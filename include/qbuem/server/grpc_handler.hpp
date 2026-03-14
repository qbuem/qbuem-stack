#pragma once

/**
 * @file qbuem/server/grpc_handler.hpp
 * @brief gRPC over HTTP/2 핸들러 — 프로토버프 비종속 Length-Prefixed 메시지 처리
 * @defgroup qbuem_grpc_handler gRPC Handler
 * @ingroup qbuem_server
 *
 * 이 헤더는 gRPC 프로토콜(gRPC over HTTP/2)의 서버 측 처리를 제공합니다.
 * 직렬화/역직렬화는 호출자가 제공하는 코덱에 위임하므로 특정 IDL 또는
 * 직렬화 라이브러리에 종속되지 않습니다.
 *
 * ### 지원하는 스트리밍 패턴 (RFC 정의)
 * - **Unary RPC**: 단일 요청 → 단일 응답.
 * - **Server Streaming RPC**: 단일 요청 → 응답 스트림.
 * - **Client Streaming RPC**: 요청 스트림 → 단일 응답.
 * - **Bidirectional Streaming RPC**: 요청 스트림 ↔ 응답 스트림.
 *
 * ### 프레임 형식
 * gRPC Length-Prefixed Message (5바이트 헤더):
 * ```
 * +---------+------------------+
 * | 1 byte  |  4 bytes (BE)    |
 * | Compressed Flag | Message Length |
 * +---------+------------------+
 * |       Message Payload      |
 * +----------------------------+
 * ```
 *
 * ### 사용 예시 (Unary RPC)
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
 * @brief gRPC Length-Prefixed Message 봉투 구조체.
 *
 * 직렬화된 페이로드를 감싸는 gRPC 메시지 봉투입니다.
 * 실제 직렬화(protobuf, flatbuffers 등)는 호출자 책임입니다.
 * `payload`는 단순한 원시 바이트 벡터이므로 어떤 직렬화 라이브러리도 사용 가능합니다.
 */
struct GrpcMessage {
  /** @brief 페이로드 압축 여부 (gRPC Compressed-Flag, 0x00 = 비압축, 0x01 = 압축). */
  bool compressed{false};

  /** @brief 직렬화된 페이로드 바이트. 애플리케이션이 protobuf 등으로 직렬화한 내용. */
  std::vector<uint8_t> payload;
};

// ─── GrpcStatus ──────────────────────────────────────────────────────────────

/**
 * @brief gRPC 상태 코드 (google.rpc.Code 매핑).
 *
 * 성공/실패 여부를 gRPC 트레일러 `grpc-status` 헤더로 전달하는 데 사용됩니다.
 * 각 값은 google.rpc.Code 숫자 값과 동일합니다.
 *
 * 참조: https://grpc.github.io/grpc/core/md_doc_statuscodes.html
 */
enum class GrpcStatus : uint32_t {
  OK                  =  0, ///< 성공
  CANCELLED           =  1, ///< 호출자에 의해 취소됨
  UNKNOWN             =  2, ///< 알 수 없는 에러
  INVALID_ARGUMENT    =  3, ///< 유효하지 않은 인자
  NOT_FOUND           =  5, ///< 요청한 리소스를 찾을 수 없음
  ALREADY_EXISTS      =  6, ///< 생성하려는 리소스가 이미 존재함
  PERMISSION_DENIED   =  7, ///< 권한 없음
  RESOURCE_EXHAUSTED  =  8, ///< 자원 부족 (할당량 초과 등)
  FAILED_PRECONDITION =  9, ///< 사전 조건 위반
  ABORTED             = 10, ///< 작업이 중단됨 (트랜잭션 충돌 등)
  INTERNAL            = 13, ///< 내부 서버 에러
  UNAVAILABLE         = 14, ///< 서비스 일시 불가 (재시도 가능)
};

// ─── Stream / AsyncChannel 전방 선언 ─────────────────────────────────────────

/**
 * @brief 서버 → 클라이언트 방향 응답 스트림 인터페이스.
 *
 * Server Streaming 및 Bidirectional Streaming RPC에서 응답을 전송할 때 사용합니다.
 * `send()`를 반복 호출하여 여러 응답 메시지를 스트리밍합니다.
 *
 * @tparam Res 응답 메시지 타입.
 */
template <typename Res>
class Stream {
public:
  virtual ~Stream() = default;

  /**
   * @brief 단일 응답 메시지를 스트림에 씁니다.
   *
   * @param response 전송할 응답 메시지.
   * @returns 성공 시 `Result<void>::ok()`, 스트림 에러 시 에러 코드.
   */
  virtual Task<Result<void>> send(Res response) = 0;

  /**
   * @brief 스트림이 아직 열려 있는지 확인합니다.
   *
   * @returns 클라이언트 연결이 활성 상태면 true.
   */
  [[nodiscard]] virtual bool is_open() const noexcept = 0;
};

/**
 * @brief 클라이언트 → 서버 방향 비동기 요청 채널.
 *
 * Client Streaming 및 Bidirectional Streaming RPC에서 클라이언트 메시지를 수신합니다.
 * `recv()`를 반복 호출하여 스트림을 소비합니다.
 *
 * @tparam Req 요청 메시지 타입.
 */
template <typename Req>
class AsyncChannel {
public:
  virtual ~AsyncChannel() = default;

  /**
   * @brief 다음 요청 메시지를 수신합니다.
   *
   * 스트림이 종료(클라이언트가 half-close)되면 `has_value() == false`인 결과를 반환합니다.
   *
   * @returns 다음 메시지. 스트림 종료 시 `std::nullopt`. 에러 시 에러 코드.
   */
  virtual Task<Result<std::optional<Req>>> recv() = 0;
};

// ─── GrpcHandler<Req, Res> ───────────────────────────────────────────────────

/**
 * @brief gRPC 메서드 핸들러 (프로토버프 비종속).
 *
 * 4가지 gRPC 스트리밍 패턴을 지원하며 직렬화/역직렬화를 외부로 위임합니다.
 * 단일 인스턴스에서 하나의 gRPC 메서드(서비스.메서드)를 처리합니다.
 *
 * ### 등록 패턴 선택
 * - Unary: `set_unary()` 사용.
 * - Server Streaming: `set_server_stream()` 사용.
 * - Client Streaming: `set_client_stream()` 사용.
 * - Bidirectional Streaming: `set_bidi()` 사용.
 *
 * 동시에 여러 패턴을 등록할 수 있지만, 실제 호출은 클라이언트 요청 유형에 따라
 * 하나만 디스패치됩니다.
 *
 * @tparam Req 요청 메시지 타입. 직렬화/역직렬화는 `DeserializeFn`이 담당합니다.
 * @tparam Res 응답 메시지 타입. 직렬화/역직렬화는 `SerializeFn`이 담당합니다.
 */
template <typename Req, typename Res>
class GrpcHandler {
public:
  // ── 스트리밍 패턴별 함수 타입 ────────────────────────────────────────────

  /**
   * @brief Unary RPC 핸들러 타입.
   *
   * 단일 요청을 받아 단일 응답을 반환합니다.
   * `Result::err()`로 gRPC 에러를 반환할 수 있습니다.
   */
  using UnaryFn = std::function<Task<Result<Res>>(Req)>;

  /**
   * @brief Server Streaming RPC 핸들러 타입.
   *
   * 단일 요청을 받아 `Stream<Res>&`를 통해 여러 응답을 전송합니다.
   * 함수가 반환되면 서버 측 스트림이 닫힙니다.
   */
  using ServerStreamFn = std::function<Task<void>(Req, Stream<Res>&)>;

  /**
   * @brief Client Streaming RPC 핸들러 타입.
   *
   * `AsyncChannel<Req>&`에서 여러 요청을 읽어 단일 응답을 반환합니다.
   */
  using ClientStreamFn = std::function<Task<Result<Res>>(AsyncChannel<Req>&)>;

  /**
   * @brief Bidirectional Streaming RPC 핸들러 타입.
   *
   * `AsyncChannel<Req>&`에서 요청을 읽으면서 `Stream<Res>&`로 응답을 전송합니다.
   */
  using BidiFn = std::function<Task<void>(AsyncChannel<Req>&, Stream<Res>&)>;

  /**
   * @brief 응답 직렬화 함수 타입.
   *
   * `Res` 객체를 원시 바이트 벡터로 변환합니다.
   * protobuf의 경우: `[](const Res& r){ return r.SerializeAsString(); }`
   */
  using SerializeFn = std::function<std::vector<uint8_t>(const Res&)>;

  /**
   * @brief 요청 역직렬화 함수 타입.
   *
   * 원시 바이트를 `Req` 객체로 변환합니다.
   * protobuf의 경우: `[](std::span<const uint8_t> b){ Req r; r.ParseFromArray(b.data(), b.size()); return r; }`
   */
  using DeserializeFn = std::function<Req(std::span<const uint8_t>)>;

  // ── 설정 구조체 ──────────────────────────────────────────────────────────

  /**
   * @brief GrpcHandler 설정 구조체.
   */
  struct Config {
    /** @brief gRPC 서비스 이름 (패키지 포함, 예: "mypackage.MyService"). */
    std::string service_name;
    /** @brief gRPC 메서드 이름 (예: "SayHello"). */
    std::string method_name;
    /** @brief 최대 수신 메시지 크기 (bytes). 초과 시 RESOURCE_EXHAUSTED 반환. */
    uint32_t max_recv_message_size{4 * 1024 * 1024}; // 기본값 4MB
    /** @brief 최대 전송 메시지 크기 (bytes). */
    uint32_t max_send_message_size{4 * 1024 * 1024}; // 기본값 4MB
  };

  /**
   * @brief GrpcHandler를 구성합니다.
   *
   * @param cfg 핸들러 설정.
   */
  explicit GrpcHandler(Config cfg) : cfg_(std::move(cfg)) {}

  // ── 핸들러 등록 (메서드 체이닝) ─────────────────────────────────────────

  /**
   * @brief Unary RPC 핸들러를 등록합니다.
   *
   * @param fn Unary 핸들러 함수.
   * @returns 메서드 체이닝을 위한 `*this` 참조.
   */
  GrpcHandler& set_unary(UnaryFn fn) {
    unary_fn_ = std::move(fn);
    return *this;
  }

  /**
   * @brief Server Streaming RPC 핸들러를 등록합니다.
   *
   * @param fn Server Streaming 핸들러 함수.
   * @returns 메서드 체이닝을 위한 `*this` 참조.
   */
  GrpcHandler& set_server_stream(ServerStreamFn fn) {
    server_stream_fn_ = std::move(fn);
    return *this;
  }

  /**
   * @brief Client Streaming RPC 핸들러를 등록합니다.
   *
   * @param fn Client Streaming 핸들러 함수.
   * @returns 메서드 체이닝을 위한 `*this` 참조.
   */
  GrpcHandler& set_client_stream(ClientStreamFn fn) {
    client_stream_fn_ = std::move(fn);
    return *this;
  }

  /**
   * @brief Bidirectional Streaming RPC 핸들러를 등록합니다.
   *
   * @param fn Bidirectional Streaming 핸들러 함수.
   * @returns 메서드 체이닝을 위한 `*this` 참조.
   */
  GrpcHandler& set_bidi(BidiFn fn) {
    bidi_fn_ = std::move(fn);
    return *this;
  }

  /**
   * @brief 직렬화/역직렬화 함수를 등록합니다.
   *
   * 응답 직렬화(`SerializeFn`)와 요청 역직렬화(`DeserializeFn`)를 함께 설정합니다.
   * 이 함수를 호출하지 않으면 `dispatch_unary()` 등의 호출이 에러를 반환합니다.
   *
   * @param s 응답 직렬화 함수.
   * @param d 요청 역직렬화 함수.
   * @returns 메서드 체이닝을 위한 `*this` 참조.
   */
  GrpcHandler& set_serializer(SerializeFn s, DeserializeFn d) {
    serialize_fn_   = std::move(s);
    deserialize_fn_ = std::move(d);
    return *this;
  }

  // ── RPC 디스패치 ─────────────────────────────────────────────────────────

  /**
   * @brief Unary RPC를 디스패치합니다.
   *
   * `GrpcMessage` 봉투에서 요청을 역직렬화하고, 등록된 `UnaryFn`을 호출한 후,
   * 응답을 `GrpcMessage` 봉투로 직렬화합니다.
   *
   * @param request 클라이언트로부터 수신한 원시 gRPC 메시지.
   * @returns 직렬화된 응답 메시지. 에러 시 에러 코드.
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
   * @brief Server Streaming RPC를 디스패치합니다.
   *
   * 단일 요청을 역직렬화하고, `stream`을 통해 여러 응답을 전송합니다.
   *
   * @param request 클라이언트 요청 메시지.
   * @param stream  응답 전송에 사용할 서버 스트림.
   * @returns 성공 시 `Result<void>::ok()`.
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
    co_return Result<void>::ok();
  }

  /**
   * @brief Client Streaming RPC를 디스패치합니다.
   *
   * `channel`에서 여러 요청을 읽고 단일 응답 메시지를 반환합니다.
   *
   * @param channel 클라이언트 요청 스트림.
   * @returns 직렬화된 단일 응답. 에러 시 에러 코드.
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
   * @brief Bidirectional Streaming RPC를 디스패치합니다.
   *
   * `channel`에서 요청을 읽으면서 `stream`으로 응답을 동시에 전송합니다.
   *
   * @param channel 클라이언트 요청 스트림.
   * @param stream  서버 응답 스트림.
   * @returns 성공 시 `Result<void>::ok()`.
   */
  Task<Result<void>> dispatch_bidi(AsyncChannel<Req>& channel,
                                    Stream<Res>&        stream) {
    if (!bidi_fn_) {
      co_return unexpected(
          std::make_error_code(std::errc::function_not_supported));
    }

    co_await bidi_fn_(channel, stream);
    co_return Result<void>::ok();
  }

  // ── gRPC 프레임 직렬화/역직렬화 ─────────────────────────────────────────

  /**
   * @brief `GrpcMessage`를 Length-Prefixed 5바이트 헤더 + 페이로드로 인코딩합니다.
   *
   * 형식: [Compressed-Flag(1B)][Message-Length(4B, BE)][Payload]
   *
   * @param msg 인코딩할 gRPC 메시지.
   * @returns 인코딩된 바이트 벡터.
   */
  static std::vector<uint8_t> encode_message(const GrpcMessage& msg) {
    std::vector<uint8_t> out;
    out.reserve(5 + msg.payload.size());

    // Compressed-Flag (0x00 = 비압축, 0x01 = 압축)
    out.push_back(msg.compressed ? 0x01u : 0x00u);

    // Message-Length (4바이트 빅엔디언)
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
   * @brief Length-Prefixed 바이트 스팬에서 `GrpcMessage`를 디코딩합니다.
   *
   * 데이터가 부족하면 `has_value() == false`인 결과를 반환합니다.
   *
   * @param data     디코딩할 바이트 범위.
   * @param consumed 성공 시 소비한 바이트 수.
   * @returns 디코딩된 GrpcMessage. 데이터 부족 시 에러 코드.
   */
  static Result<GrpcMessage> decode_message(std::span<const uint8_t> data,
                                             size_t& consumed) {
    consumed = 0;

    // 5바이트 헤더 필요
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

  // ── gRPC 트레일러 헬퍼 ──────────────────────────────────────────────────

  /**
   * @brief gRPC 상태 트레일러 문자열을 생성합니다.
   *
   * HTTP/2 gRPC 응답의 마지막 HEADERS 프레임에 포함되는 트레일러 블록입니다.
   *
   * 형식:
   * ```
   * grpc-status: <code>\r\n
   * grpc-message: <message>\r\n   (message가 비어 있지 않을 때만)
   * ```
   *
   * @param code    gRPC 상태 코드.
   * @param message 선택적 사람이 읽을 수 있는 에러 메시지.
   * @returns 트레일러 헤더 문자열.
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

  // ── 접근자 ──────────────────────────────────────────────────────────────

  /**
   * @brief gRPC 경로 문자열을 반환합니다.
   *
   * gRPC HTTP/2 요청의 `:path` 의사 헤더 형식입니다.
   * 형식: `/<service_name>/<method_name>`
   *
   * @returns gRPC 경로 문자열.
   */
  [[nodiscard]] std::string grpc_path() const {
    return "/" + cfg_.service_name + "/" + cfg_.method_name;
  }

  /** @brief 현재 설정을 반환합니다. */
  [[nodiscard]] const Config& config() const noexcept { return cfg_; }

  /** @brief Unary 핸들러가 등록되어 있으면 true를 반환합니다. */
  [[nodiscard]] bool has_unary() const noexcept {
    return static_cast<bool>(unary_fn_);
  }

  /** @brief Server Streaming 핸들러가 등록되어 있으면 true를 반환합니다. */
  [[nodiscard]] bool has_server_stream() const noexcept {
    return static_cast<bool>(server_stream_fn_);
  }

  /** @brief Client Streaming 핸들러가 등록되어 있으면 true를 반환합니다. */
  [[nodiscard]] bool has_client_stream() const noexcept {
    return static_cast<bool>(client_stream_fn_);
  }

  /** @brief Bidirectional Streaming 핸들러가 등록되어 있으면 true를 반환합니다. */
  [[nodiscard]] bool has_bidi() const noexcept {
    return static_cast<bool>(bidi_fn_);
  }

private:
  /** @brief 핸들러 설정. */
  Config cfg_;

  /** @brief Unary RPC 핸들러. 미등록 시 nullptr. */
  UnaryFn unary_fn_;

  /** @brief Server Streaming RPC 핸들러. 미등록 시 nullptr. */
  ServerStreamFn server_stream_fn_;

  /** @brief Client Streaming RPC 핸들러. 미등록 시 nullptr. */
  ClientStreamFn client_stream_fn_;

  /** @brief Bidirectional Streaming RPC 핸들러. 미등록 시 nullptr. */
  BidiFn bidi_fn_;

  /** @brief 응답 직렬화 함수. `set_serializer()` 호출 전에는 nullptr. */
  SerializeFn serialize_fn_;

  /** @brief 요청 역직렬화 함수. `set_serializer()` 호출 전에는 nullptr. */
  DeserializeFn deserialize_fn_;
};

} // namespace qbuem

/** @} */ // end of qbuem_grpc_handler
