#pragma once

/**
 * @file qbuem/server/grpc_handler.hpp
 * @brief gRPC over HTTP/2 handler — protobuf-independent Length-Prefixed message processing
 * @defgroup qbuem_grpc_handler gRPC Handler
 * @ingroup qbuem_server
 *
 * This header provides server-side processing of the gRPC protocol (gRPC over HTTP/2).
 * Serialization/deserialization is delegated to a caller-supplied codec, so there is
 * no dependency on any specific IDL or serialization library.
 *
 * ### Supported streaming patterns (RFC-defined)
 * - **Unary RPC**: single request → single response.
 * - **Server Streaming RPC**: single request → response stream.
 * - **Client Streaming RPC**: request stream → single response.
 * - **Bidirectional Streaming RPC**: request stream ↔ response stream.
 *
 * ### Frame format
 * gRPC Length-Prefixed Message (5-byte header):
 * ```
 * +---------+------------------+
 * | 1 byte  |  4 bytes (BE)    |
 * | Compressed Flag | Message Length |
 * +---------+------------------+
 * |       Message Payload      |
 * +----------------------------+
 * ```
 *
 * ### Usage example (Unary RPC)
 * @code
 * GrpcHandler<MyRequest, MyResponse> handler({
 *     .service_name = "mypackage.MyService",
 *     .method_name  = "SayHello",
 * });
 * handler.set_serializer(
 *     [](const MyResponse& r) { return r.SerializeAsBytes(); },
 *     [](std::span<const uint8_t> b) {
 *         MyRequest req; req.ParseFromBytes(b); return req;
 *     }
 * );
 * handler.set_unary([](MyRequest req) -> Task<Result<MyResponse>> {
 *     MyResponse res;
 *     res.set_message("Hello, " + req.name() + "!");
 *     co_return res;
 * });
 * @endcode
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace qbuem {

// ─── GrpcMessage ─────────────────────────────────────────────────────────────

/**
 * @brief gRPC Length-Prefixed Message envelope struct.
 *
 * A gRPC message envelope wrapping a serialized payload.
 * Actual serialization (protobuf, flatbuffers, etc.) is the caller's responsibility.
 * `payload` is a plain raw byte vector, so any serialization library may be used.
 */
struct GrpcMessage {
  /** @brief Whether the payload is compressed (gRPC Compressed-Flag, 0x00 = uncompressed, 0x01 = compressed). */
  bool compressed{false};

  /** @brief Serialized payload bytes — content serialized by the application using protobuf etc. */
  std::vector<uint8_t> payload;
};

// ─── GrpcStatus ──────────────────────────────────────────────────────────────

/**
 * @brief gRPC status codes (google.rpc.Code mapping).
 *
 * Used to convey success/failure via the gRPC trailer `grpc-status` header.
 * Each value equals the corresponding google.rpc.Code numeric value.
 *
 * Reference: https://grpc.github.io/grpc/core/md_doc_statuscodes.html
 */
enum class GrpcStatus : uint32_t {
  OK                  =  0, ///< Success
  CANCELLED           =  1, ///< Cancelled by the caller
  UNKNOWN             =  2, ///< Unknown error
  INVALID_ARGUMENT    =  3, ///< Invalid argument
  NOT_FOUND           =  5, ///< Requested resource not found
  ALREADY_EXISTS      =  6, ///< Resource being created already exists
  PERMISSION_DENIED   =  7, ///< Permission denied
  RESOURCE_EXHAUSTED  =  8, ///< Resource exhausted (quota exceeded, etc.)
  FAILED_PRECONDITION =  9, ///< Precondition violated
  ABORTED             = 10, ///< Operation aborted (transaction conflict, etc.)
  INTERNAL            = 13, ///< Internal server error
  UNAVAILABLE         = 14, ///< Service temporarily unavailable (retryable)
};

// ─── Stream / AsyncChannel forward declarations ───────────────────────────────

/**
 * @brief Server → client response stream interface.
 *
 * Used to send responses in Server Streaming and Bidirectional Streaming RPCs.
 * Call `send()` repeatedly to stream multiple response messages.
 *
 * @tparam Res Response message type.
 */
template <typename Res>
class Stream {
public:
  virtual ~Stream() = default;

  /**
   * @brief Writes a single response message to the stream.
   *
   * @param response Response message to send.
   * @returns `Result<void>{}` on success, or an error code on stream error.
   */
  virtual Task<Result<void>> send(Res response) = 0;

  /**
   * @brief Checks whether the stream is still open.
   *
   * @returns true if the client connection is active.
   */
  [[nodiscard]] virtual bool is_open() const noexcept = 0;
};

/**
 * @brief Client → server asynchronous request channel.
 *
 * Used to receive client messages in Client Streaming and Bidirectional Streaming RPCs.
 * Call `recv()` repeatedly to consume the stream.
 *
 * @tparam Req Request message type.
 */
template <typename Req>
class AsyncChannel {
public:
  virtual ~AsyncChannel() = default;

  /**
   * @brief Receives the next request message.
   *
   * Returns a result with `has_value() == false` when the stream ends (client half-close).
   *
   * @returns Next message, `std::nullopt` on stream end, or an error code on error.
   */
  virtual Task<Result<std::optional<Req>>> recv() = 0;
};

// ─── GrpcHandler<Req, Res> ───────────────────────────────────────────────────

/**
 * @brief gRPC method handler (protobuf-independent).
 *
 * Supports all 4 gRPC streaming patterns and delegates serialization/deserialization externally.
 * A single instance handles one gRPC method (service.method).
 *
 * ### Choosing a registration pattern
 * - Unary: use `set_unary()`.
 * - Server Streaming: use `set_server_stream()`.
 * - Client Streaming: use `set_client_stream()`.
 * - Bidirectional Streaming: use `set_bidi()`.
 *
 * Multiple patterns may be registered simultaneously, but only one is dispatched
 * per call based on the client request type.
 *
 * @tparam Req Request message type. Serialization/deserialization is handled by `DeserializeFn`.
 * @tparam Res Response message type. Serialization/deserialization is handled by `SerializeFn`.
 */
template <typename Req, typename Res>
class GrpcHandler {
public:
  // ── Function types per streaming pattern ─────────────────────────────────

  /**
   * @brief Unary RPC handler type.
   *
   * Receives a single request and returns a single response.
   * gRPC errors may be returned via `Result::err()`.
   */
  using UnaryFn = std::function<Task<Result<Res>>(Req)>;

  /**
   * @brief Server Streaming RPC handler type.
   *
   * Receives a single request and sends multiple responses via `Stream<Res>&`.
   * The server-side stream is closed when the function returns.
   */
  using ServerStreamFn = std::function<Task<void>(Req, Stream<Res>&)>;

  /**
   * @brief Client Streaming RPC handler type.
   *
   * Reads multiple requests from `AsyncChannel<Req>&` and returns a single response.
   */
  using ClientStreamFn = std::function<Task<Result<Res>>(AsyncChannel<Req>&)>;

  /**
   * @brief Bidirectional Streaming RPC handler type.
   *
   * Reads requests from `AsyncChannel<Req>&` while sending responses via `Stream<Res>&`.
   */
  using BidiFn = std::function<Task<void>(AsyncChannel<Req>&, Stream<Res>&)>;

  /**
   * @brief Response serialization function type.
   *
   * Converts a `Res` object to a raw byte vector.
   * For protobuf: `[](const Res& r){ return r.SerializeAsString(); }`
   */
  using SerializeFn = std::function<std::vector<uint8_t>(const Res&)>;

  /**
   * @brief Request deserialization function type.
   *
   * Converts raw bytes to a `Req` object.
   * For protobuf: `[](std::span<const uint8_t> b){ Req r; r.ParseFromArray(b.data(), b.size()); return r; }`
   */
  using DeserializeFn = std::function<Req(std::span<const uint8_t>)>;

  // ── Configuration struct ──────────────────────────────────────────────────

  /**
   * @brief GrpcHandler configuration struct.
   */
  struct Config {
    /** @brief gRPC service name (including package, e.g. "mypackage.MyService"). */
    std::string service_name;
    /** @brief gRPC method name (e.g. "SayHello"). */
    std::string method_name;
    /** @brief Maximum receive message size (bytes). Returns RESOURCE_EXHAUSTED if exceeded. */
    uint32_t max_recv_message_size{4 * 1024 * 1024}; // default 4MB
    /** @brief Maximum send message size (bytes). */
    uint32_t max_send_message_size{4 * 1024 * 1024}; // default 4MB
  };

  /**
   * @brief Constructs a GrpcHandler.
   *
   * @param cfg Handler configuration.
   */
  explicit GrpcHandler(Config cfg) : cfg_(std::move(cfg)) {}

  // ── Handler registration (method chaining) ───────────────────────────────

  /**
   * @brief Registers a Unary RPC handler.
   *
   * @param fn Unary handler function.
   * @returns `*this` reference for method chaining.
   */
  GrpcHandler& set_unary(UnaryFn fn) {
    unary_fn_ = std::move(fn);
    return *this;
  }

  /**
   * @brief Registers a Server Streaming RPC handler.
   *
   * @param fn Server Streaming handler function.
   * @returns `*this` reference for method chaining.
   */
  GrpcHandler& set_server_stream(ServerStreamFn fn) {
    server_stream_fn_ = std::move(fn);
    return *this;
  }

  /**
   * @brief Registers a Client Streaming RPC handler.
   *
   * @param fn Client Streaming handler function.
   * @returns `*this` reference for method chaining.
   */
  GrpcHandler& set_client_stream(ClientStreamFn fn) {
    client_stream_fn_ = std::move(fn);
    return *this;
  }

  /**
   * @brief Registers a Bidirectional Streaming RPC handler.
   *
   * @param fn Bidirectional Streaming handler function.
   * @returns `*this` reference for method chaining.
   */
  GrpcHandler& set_bidi(BidiFn fn) {
    bidi_fn_ = std::move(fn);
    return *this;
  }

  /**
   * @brief Registers serialization/deserialization functions.
   *
   * Sets the response serializer (`SerializeFn`) and request deserializer (`DeserializeFn`) together.
   * If this function is not called, calls to `dispatch_unary()` etc. will return an error.
   *
   * @param s Response serialization function.
   * @param d Request deserialization function.
   * @returns `*this` reference for method chaining.
   */
  GrpcHandler& set_serializer(SerializeFn s, DeserializeFn d) {
    serialize_fn_   = std::move(s);
    deserialize_fn_ = std::move(d);
    return *this;
  }

  // ── RPC dispatch ─────────────────────────────────────────────────────────

  /**
   * @brief Dispatches a Unary RPC.
   *
   * Deserializes the request from a `GrpcMessage` envelope, invokes the registered `UnaryFn`,
   * then serializes the response back into a `GrpcMessage` envelope.
   *
   * @param request Raw gRPC message received from the client.
   * @returns Serialized response message, or an error code on failure.
   */
  Task<Result<GrpcMessage>> dispatch_unary(const GrpcMessage& request) {
    if (!serialize_fn_ || !deserialize_fn_) {
      co_return unexpected(
          std::make_error_code(std::errc::function_not_supported));
    }
    if (!unary_fn_) {
      co_return unexpected(
          std::make_error_code(std::errc::function_not_supported));
    }
    if (request.payload.size() > cfg_.max_recv_message_size) {
      co_return unexpected(
          std::make_error_code(std::errc::message_size));
    }

    Req req = deserialize_fn_(std::span<const uint8_t>(request.payload));
    auto result = co_await unary_fn_(std::move(req));
    if (!result) {
      co_return unexpected(result.error());
    }

    auto payload = serialize_fn_(result.value());
    if (payload.size() > cfg_.max_send_message_size) {
      co_return unexpected(
          std::make_error_code(std::errc::message_size));
    }

    co_return GrpcMessage{false, std::move(payload)};
  }

  /**
   * @brief Dispatches a Server Streaming RPC.
   *
   * Deserializes a single request and sends multiple responses via `stream`.
   *
   * @param request Client request message.
   * @param stream  Server stream used to send responses.
   * @returns `Result<void>{}` on success.
   */
  Task<Result<void>> dispatch_server_stream(const GrpcMessage& request,
                                             Stream<Res>&        stream) {
    if (!deserialize_fn_ || !server_stream_fn_) {
      co_return unexpected(
          std::make_error_code(std::errc::function_not_supported));
    }
    if (request.payload.size() > cfg_.max_recv_message_size) {
      co_return unexpected(
          std::make_error_code(std::errc::message_size));
    }

    Req req = deserialize_fn_(std::span<const uint8_t>(request.payload));
    co_await server_stream_fn_(std::move(req), stream);
    co_return Result<void>{};
  }

  /**
   * @brief Dispatches a Client Streaming RPC.
   *
   * Reads multiple requests from `channel` and returns a single response message.
   *
   * @param channel Client request stream.
   * @returns Serialized single response, or an error code on failure.
   */
  Task<Result<GrpcMessage>> dispatch_client_stream(AsyncChannel<Req>& channel) {
    if (!serialize_fn_ || !client_stream_fn_) {
      co_return unexpected(
          std::make_error_code(std::errc::function_not_supported));
    }

    auto result = co_await client_stream_fn_(channel);
    if (!result) {
      co_return unexpected(result.error());
    }

    auto payload = serialize_fn_(result.value());
    if (payload.size() > cfg_.max_send_message_size) {
      co_return unexpected(
          std::make_error_code(std::errc::message_size));
    }

    co_return GrpcMessage{false, std::move(payload)};
  }

  /**
   * @brief Dispatches a Bidirectional Streaming RPC.
   *
   * Reads requests from `channel` while simultaneously sending responses via `stream`.
   *
   * @param channel Client request stream.
   * @param stream  Server response stream.
   * @returns `Result<void>{}` on success.
   */
  Task<Result<void>> dispatch_bidi(AsyncChannel<Req>& channel,
                                    Stream<Res>&        stream) {
    if (!bidi_fn_) {
      co_return unexpected(
          std::make_error_code(std::errc::function_not_supported));
    }

    co_await bidi_fn_(channel, stream);
    co_return Result<void>{};
  }

  // ── gRPC frame serialization/deserialization ─────────────────────────────

  /**
   * @brief Encodes a `GrpcMessage` as a Length-Prefixed 5-byte header + payload.
   *
   * Format: [Compressed-Flag(1B)][Message-Length(4B, BE)][Payload]
   *
   * @param msg gRPC message to encode.
   * @returns Encoded byte vector.
   */
  static std::vector<uint8_t> encode_message(const GrpcMessage& msg) {
    std::vector<uint8_t> out;
    out.reserve(5 + msg.payload.size());

    // Compressed-Flag (0x00 = uncompressed, 0x01 = compressed)
    out.push_back(msg.compressed ? 0x01u : 0x00u);

    // Message-Length (4-byte big-endian)
    uint32_t len = static_cast<uint32_t>(msg.payload.size());
    out.push_back(static_cast<uint8_t>(len >> 24u));
    out.push_back(static_cast<uint8_t>(len >> 16u));
    out.push_back(static_cast<uint8_t>(len >>  8u));
    out.push_back(static_cast<uint8_t>(len       ));

    // Payload
    out.insert(out.end(), msg.payload.begin(), msg.payload.end());
    return out;
  }

  /**
   * @brief Decodes a `GrpcMessage` from a Length-Prefixed byte span.
   *
   * Returns a result with `has_value() == false` if data is insufficient.
   *
   * @param data     Byte range to decode.
   * @param consumed Number of bytes consumed on success.
   * @returns Decoded GrpcMessage, or an error code if data is insufficient.
   */
  static Result<GrpcMessage> decode_message(std::span<const uint8_t> data,
                                             size_t& consumed) {
    consumed = 0;

    // 5-byte header required
    if (data.size() < 5) {
      return unexpected(
          std::make_error_code(std::errc::resource_unavailable_try_again));
    }

    GrpcMessage msg;
    msg.compressed = (data[0] == 0x01u);

    uint32_t payload_len =
        (static_cast<uint32_t>(data[1]) << 24u) |
        (static_cast<uint32_t>(data[2]) << 16u) |
        (static_cast<uint32_t>(data[3]) <<  8u) |
        (static_cast<uint32_t>(data[4]));

    if (data.size() < 5u + payload_len) {
      return unexpected(
          std::make_error_code(std::errc::resource_unavailable_try_again));
    }

    msg.payload.assign(data.data() + 5,
                       data.data() + 5 + payload_len);
    consumed = 5u + static_cast<size_t>(payload_len);
    return msg;
  }

  // ── gRPC trailer helpers ─────────────────────────────────────────────────

  /**
   * @brief Generates a gRPC status trailer string.
   *
   * This is the trailer block included in the final HEADERS frame of an HTTP/2 gRPC response.
   *
   * Format:
   * ```
   * grpc-status: <code>\r\n
   * grpc-message: <message>\r\n   (only when message is non-empty)
   * ```
   *
   * @param code    gRPC status code.
   * @param message Optional human-readable error message.
   * @returns Trailer header string.
   */
  static std::string status_trailer(GrpcStatus        code,
                                     std::string_view  message = "") {
    std::string trailer;
    trailer.reserve(64);
    trailer += "grpc-status: ";
    trailer += std::to_string(static_cast<uint32_t>(code));
    trailer += "\r\n";
    if (!message.empty()) {
      trailer += "grpc-message: ";
      trailer.append(message);
      trailer += "\r\n";
    }
    return trailer;
  }

  // ── Accessors ────────────────────────────────────────────────────────────

  /**
   * @brief Returns the gRPC path string.
   *
   * This is the `:path` pseudo-header format for gRPC HTTP/2 requests.
   * Format: `/<service_name>/<method_name>`
   *
   * @returns gRPC path string.
   */
  [[nodiscard]] std::string grpc_path() const {
    return "/" + cfg_.service_name + "/" + cfg_.method_name;
  }

  /** @brief Returns the current configuration. */
  [[nodiscard]] const Config& config() const noexcept { return cfg_; }

  /** @brief Returns true if a Unary handler is registered. */
  [[nodiscard]] bool has_unary() const noexcept {
    return static_cast<bool>(unary_fn_);
  }

  /** @brief Returns true if a Server Streaming handler is registered. */
  [[nodiscard]] bool has_server_stream() const noexcept {
    return static_cast<bool>(server_stream_fn_);
  }

  /** @brief Returns true if a Client Streaming handler is registered. */
  [[nodiscard]] bool has_client_stream() const noexcept {
    return static_cast<bool>(client_stream_fn_);
  }

  /** @brief Returns true if a Bidirectional Streaming handler is registered. */
  [[nodiscard]] bool has_bidi() const noexcept {
    return static_cast<bool>(bidi_fn_);
  }

private:
  /** @brief Handler configuration. */
  Config cfg_;

  /** @brief Unary RPC handler. nullptr if not registered. */
  UnaryFn unary_fn_;

  /** @brief Server Streaming RPC handler. nullptr if not registered. */
  ServerStreamFn server_stream_fn_;

  /** @brief Client Streaming RPC handler. nullptr if not registered. */
  ClientStreamFn client_stream_fn_;

  /** @brief Bidirectional Streaming RPC handler. nullptr if not registered. */
  BidiFn bidi_fn_;

  /** @brief Response serialization function. nullptr before `set_serializer()` is called. */
  SerializeFn serialize_fn_;

  /** @brief Request deserialization function. nullptr before `set_serializer()` is called. */
  DeserializeFn deserialize_fn_;
};

} // namespace qbuem

/** @} */ // end of qbuem_grpc_handler
