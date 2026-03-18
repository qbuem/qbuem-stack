#pragma once

/**
 * @file qbuem/grpc/pipeline_integration.hpp
 * @brief gRPC ↔ Pipeline integration adapter.
 * @defgroup qbuem_grpc_pipeline gRPC Pipeline Integration
 * @ingroup qbuem_grpc
 *
 * This header provides adapter types and factory functions that connect
 * gRPC streaming contexts to qbuem-stack pipeline channels.
 *
 * Uses only the standard library and qbuem internal types — no external dependencies.
 *
 * ### Provided Components
 * - `Stream<T>`         : Server → client unidirectional streaming type (coroutine-based)
 * - `BidiEnvelope<Req,Res>` : Bidirectional streaming envelope struct
 * - `grpc_server_streaming_to_pipeline()` : Server Streaming → Pipeline adapter
 * - `grpc_client_streaming_to_channel()`  : Client Streaming → AsyncChannel adapter
 * - `make_bidi_handler()`                 : Bidi handler wrapper factory
 *
 * ### Design Principles
 * - All transport logic is delegated to injected function objects (`push_fn_`, `close_fn_`).
 * - `Stream<T>` returns a coroutine Task, so backpressure is handled via `co_await`.
 * - Pipeline integration uses channel-based composition through `AsyncChannel<T>`.
 *
 * ### Usage Example
 * @code
 * // ── Server Streaming: stream pipeline results to the client ──
 * auto handler = grpc_server_streaming_to_pipeline<MyReq, MyRes>(
 *     my_pipeline_handler_fn,  // signature: Task<Result<MyRes>>(MyReq)
 *     my_pipeline              // AsyncChannel<MyRes> supplying the pipeline
 * );
 * // handler type: std::function<Task<void>(MyReq, Stream<MyRes>)>
 *
 * // ── Bidi Streaming ──
 * auto bidi = make_bidi_handler<MyReq, MyRes>(
 *     [](BidiEnvelope<MyReq, MyRes> env) -> Task<void> {
 *         while (auto msg = co_await env.incoming->recv()) {
 *             auto res = process(*msg);
 *             co_await env.outgoing.send(std::move(res));
 *         }
 *         co_await env.outgoing.close(0, "OK");
 *     }
 * );
 * @endcode
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
// <qbuem/pipeline/channel.hpp> — actual filename in project is async_channel.hpp
#include <qbuem/pipeline/async_channel.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace qbuem::grpc {

// ─── AsyncChannel forward declaration (imported from pipeline/channel.hpp) ──
// pipeline/channel.hpp defines AsyncChannel<T> in the qbuem namespace.
// This file uses qbuem::AsyncChannel<T>.

// ─── Stream<T> ───────────────────────────────────────────────────────────────

/**
 * @brief Coroutine-based server → client unidirectional streaming type.
 *
 * Used when the server sends data to the client in a gRPC Server Streaming RPC
 * or Bidirectional Streaming RPC.
 *
 * Internally delegates transport logic to two function objects (`push_fn_`, `close_fn_`),
 * so it operates independently of the actual transport layer
 * (HTTP/2 DATA frames, test mocks, in-memory buffers, etc.).
 *
 * ### State Transitions
 * - On construction: `open_ == true`.
 * - After `close()` call: `open_ == false`. Subsequent `send()` calls return an error.
 * - If `push_fn_` returns `false`, the send failed → `open_` transitions to `false`.
 *
 * ### Coroutine Usage Pattern
 * @code
 * Task<void> handle_stream(MyRequest req, Stream<MyResponse> stream) {
 *     for (int i = 0; i < 10 && stream.is_open(); ++i) {
 *         MyResponse res;
 *         res.set_value(i);
 *         auto r = co_await stream.send(std::move(res));
 *         if (!r) break;  // client disconnected
 *     }
 *     co_await stream.close(0, "OK");
 * }
 * @endcode
 *
 * @tparam T Message type to send over the stream.
 */
template <typename T>
class Stream {
public:
    /**
     * @brief Constructs a Stream from `push_fn` and `close_fn`.
     *
     * @param push_fn  Function that sends one item to the client.
     *                 Returns `true` on success, `false` if the connection was lost.
     * @param close_fn Function that sends gRPC trailers and terminates the stream.
     *                 First argument: gRPC status code (0 = OK).
     *                 Second argument: human-readable message string.
     */
    explicit Stream(std::function<bool(T)>                        push_fn,
                    std::function<void(int, std::string)>         close_fn)
        : push_fn_(std::move(push_fn))
        , close_fn_(std::move(close_fn))
        , open_(true)
    {}

    // Non-copyable (preserves single-ownership semantics of push_fn_/close_fn_)
    Stream(const Stream&)            = delete;
    Stream& operator=(const Stream&) = delete;

    // Movable
    Stream(Stream&&) noexcept            = default;
    Stream& operator=(Stream&&) noexcept = default;

    ~Stream() = default;

    // ── Public API ─────────────────────────────────────────────────────────

    /**
     * @brief Sends one item to the client.
     *
     * Calls `push_fn_` to perform the actual transmission.
     * Returns an error if the stream is closed or the send fails.
     *
     * @param value Message value to send (moved).
     * @returns `Result<void>{}` on success.
     *          `errc::broken_pipe` if the stream is already closed.
     *          `errc::connection_reset` if `push_fn_` returns `false`.
     *
     * ### Example
     * @code
     * auto r = co_await stream.send(MyResponse{});
     * if (!r) {
     *     // client disconnected or stream is closed
     * }
     * @endcode
     */
    Task<Result<void>> send(T value) {
        if (!open_) {
            co_return unexpected(
                std::make_error_code(std::errc::broken_pipe));
        }
        bool ok = push_fn_(std::move(value));
        if (!ok) {
            open_ = false;
            co_return unexpected(
                std::make_error_code(std::errc::connection_reset));
        }
        co_return Result<void>{};
    }

    /**
     * @brief Closes the stream and sends gRPC trailers.
     *
     * Sends `grpc-status` and an optional `grpc-message` trailer via `close_fn_`.
     * After this call, `is_open() == false`.
     *
     * Calling on an already-closed stream is safe but `close_fn_` will not be invoked again.
     *
     * @param grpc_status gRPC status code (0 = OK, other = error code).
     * @param message     Optional human-readable message (default: empty string).
     *
     * ### Example
     * @code
     * co_await stream.close(0, "");            // normal close
     * co_await stream.close(13, "internal error"); // INTERNAL error
     * @endcode
     */
    Task<void> close(int grpc_status = 0, std::string_view message = "") {
        if (open_) {
            open_ = false;
            close_fn_(grpc_status, std::string(message));
        }
        co_return;
    }

    /**
     * @brief Checks whether the stream is still open.
     *
     * Returns `false` after a `send()` failure or a `close()` call.
     *
     * @returns `true` if the stream is open, `false` if it is closed.
     */
    [[nodiscard]] bool is_open() const noexcept { return open_; }

private:
    /** @brief Function that pushes an item to the actual transport layer. */
    std::function<bool(T)>                push_fn_;
    /** @brief Function that terminates the stream and sends gRPC trailers. */
    std::function<void(int, std::string)> close_fn_;
    /** @brief Flag indicating whether the stream is open. */
    bool open_{true};
};

// ─── BidiEnvelope<Req, Res> ──────────────────────────────────────────────────

/**
 * @brief Bidirectional streaming envelope — bundles the client receive channel and server send stream.
 *
 * A single entry struct passed to gRPC Bidirectional Streaming RPC handlers.
 *
 * - `incoming` : Client → server direction. Consume via `co_await incoming->recv()`.
 *   Returns `std::nullopt` on EOS (channel closed).
 * - `outgoing`  : Server → client direction. Send via `co_await outgoing.send(res)`.
 *
 * ### Usage Example
 * @code
 * auto handler = make_bidi_handler<ChatMsg, ChatMsg>(
 *     [](BidiEnvelope<ChatMsg, ChatMsg> env) -> Task<void> {
 *         while (true) {
 *             auto item = co_await env.incoming->recv();
 *             if (!item) break;              // client stream ended
 *             ChatMsg echo = *item;
 *             echo.set_prefix("[echo] ");
 *             auto r = co_await env.outgoing.send(std::move(echo));
 *             if (!r) break;                 // send failed (connection dropped)
 *         }
 *         co_await env.outgoing.close(0, "");
 *     }
 * );
 * @endcode
 *
 * @tparam Req Client → server request message type.
 * @tparam Res Server → client response message type.
 */
template <typename Req, typename Res>
struct BidiEnvelope {
    /**
     * @brief Client → server direction asynchronous receive channel.
     *
     * Receives client messages in order via `AsyncChannel<Req>::recv()`.
     * Returns EOS (`std::nullopt`) when the channel is closed.
     */
    std::shared_ptr<::qbuem::AsyncChannel<Req>> incoming;

    /**
     * @brief Server → client direction streaming channel.
     *
     * Sends messages to the client via `Stream<Res>::send()`.
     * After sending is complete, transmit gRPC trailers via `close()`.
     */
    Stream<Res> outgoing;
};

// ─── Adapter factory functions ───────────────────────────────────────────────

/**
 * @brief Creates an adapter that connects a Server Streaming RPC handler to a pipeline.
 *
 * Calls `handler_fn` to forward the request to the pipeline, then reads results
 * from the `pipeline` channel and streams them to the client via `Stream<Res>`.
 *
 * Signature of the returned function:
 * ```cpp
 * Task<void>(Req request, Stream<Res> stream)
 * ```
 *
 * ### Operation Flow
 * 1. Call `handler_fn(request, pipeline)` — forwards the request to the pipeline channel.
 * 2. `pipeline.recv()` loop — receives results produced by the pipeline one at a time.
 * 3. Deliver each result to the client via `stream.send(result)`.
 * 4. Call `stream.close(0, "OK")` when the pipeline reaches EOS or the stream is closed.
 *
 * ### Usage Example
 * @code
 * // Pipeline: one request → multiple responses
 * auto pipeline = std::make_shared<AsyncChannel<MyResponse>>(256);
 *
 * auto grpc_handler = grpc_server_streaming_to_pipeline<MyRequest, MyResponse>(
 *     [](MyRequest req, std::shared_ptr<AsyncChannel<MyResponse>> ch) -> Task<void> {
 *         for (auto& item : process(req)) {
 *             co_await ch->send(item);
 *         }
 *         ch->close();
 *     },
 *     pipeline
 * );
 *
 * // grpc_handler: std::function<Task<void>(MyRequest, Stream<MyResponse>)>
 * co_await grpc_handler(request, std::move(stream));
 * @endcode
 *
 * @tparam Req Request message type.
 * @tparam Res Response message type.
 * @param handler_fn Coroutine function that forwards the request to the pipeline channel.
 *                   Signature: `Task<void>(Req, std::shared_ptr<AsyncChannel<Res>>)`.
 * @param pipeline   Shared async channel that receives pipeline results.
 * @returns gRPC handler in the form `std::function<Task<void>(Req, Stream<Res>)>`.
 */
template <typename Req, typename Res>
std::function<Task<void>(Req, Stream<Res>)>
grpc_server_streaming_to_pipeline(
    std::function<Task<void>(Req, std::shared_ptr<::qbuem::AsyncChannel<Res>>)> handler_fn,
    std::shared_ptr<::qbuem::AsyncChannel<Res>>                                  pipeline)
{
    return [handler_fn = std::move(handler_fn),
            pipeline   = std::move(pipeline)](Req request, Stream<Res> stream) -> Task<void> {
        // Forward the request to the pipeline
        co_await handler_fn(std::move(request), pipeline);

        // Read results from the pipeline and deliver them to the client stream
        while (stream.is_open()) {
            auto item = co_await pipeline->recv();
            if (!item) {
                // EOS: pipeline channel is closed
                break;
            }
            auto r = co_await stream.send(std::move(*item));
            if (!r) {
                // Client connection dropped
                break;
            }
        }

        // Send trailers (no-op if already closed)
        co_await stream.close(0, "OK");
    };
}

/**
 * @brief Creates an `AsyncChannel<Req>` for a Client Streaming RPC and returns a feeder handler.
 *
 * Also creates a handler function that forwards messages received in a client
 * streaming RPC to an `AsyncChannel<Req>` in order.
 *
 * Return type:
 * ```cpp
 * struct {
 *     std::shared_ptr<AsyncChannel<Req>> channel;   // consumer calls recv()
 *     std::function<Task<void>(Req)>     feeder;    // called for each received message
 * };
 * ```
 *
 * ### Usage example
 * @code
 * auto [channel, feeder] = grpc_client_streaming_to_channel<MyRequest, MyResponse>();
 *
 * // gRPC framework calls feeder for each received message
 * co_await feeder(incoming_request);
 *
 * // Consumer side (separate coroutine):
 * while (auto msg = co_await channel->recv()) {
 *     process(*msg);
 * }
 * @endcode
 *
 * @tparam Req Request message type sent by the client.
 * @tparam Res Response message type returned by the server (used only for channel type tagging).
 * @param capacity Internal ring buffer capacity (default: 256).
 * @returns Struct containing `channel` and `feeder`.
 */
template <typename Req, typename Res = void>
struct ClientStreamingPair {
    /** @brief Channel from which the consumer reads messages via `recv()`. */
    std::shared_ptr<::qbuem::AsyncChannel<Req>> channel;
    /**
     * @brief Feeder function called by the gRPC framework for each received message.
     *        Propagates EOS internally when the channel is closed.
     */
    std::function<Task<void>(Req)>              feeder;
};

/**
 * @brief Creates an `AsyncChannel<Req>` + feeder handler pair.
 *
 * @see ClientStreamingPair
 *
 * @tparam Req Client request message type.
 * @tparam Res Response message type (for tagging only).
 * @param capacity Channel ring buffer capacity (default: 256).
 * @returns `ClientStreamingPair<Req, Res>`.
 *
 * ### Usage example
 * @code
 * auto pair = grpc_client_streaming_to_channel<UploadChunk, UploadResult>(512);
 *
 * // In the gRPC receive loop:
 * while (has_next_frame()) {
 *     auto chunk = decode_chunk(recv_frame());
 *     co_await pair.feeder(std::move(chunk));
 * }
 * pair.channel->close();  // signal EOS
 *
 * // In the processing coroutine:
 * while (auto chunk = co_await pair.channel->recv()) {
 *     append_to_storage(*chunk);
 * }
 * @endcode
 */
template <typename Req, typename Res = void>
ClientStreamingPair<Req, Res>
grpc_client_streaming_to_channel(std::size_t capacity = 256)
{
    auto channel = std::make_shared<::qbuem::AsyncChannel<Req>>(capacity);

    auto feeder = [ch = channel](Req req) -> Task<void> {
        co_await ch->send(std::move(req));
    };

    return ClientStreamingPair<Req, Res>{
        std::move(channel),
        std::move(feeder)
    };
}

/**
 * @brief Wraps a Bidirectional Streaming RPC handler using `BidiEnvelope`.
 *
 * Accepts `handler_fn` and converts it into a callable in the form
 * `std::function<Task<void>(std::shared_ptr<AsyncChannel<Req>>, Stream<Res>)>`
 * expected by the gRPC Bidirectional Streaming RPC framework.
 *
 * Constructs a `BidiEnvelope<Req, Res>` internally and passes it to `handler_fn`.
 *
 * ### Usage example
 * @code
 * auto bidi = make_bidi_handler<ChatMessage, ChatMessage>(
 *     [](BidiEnvelope<ChatMessage, ChatMessage> env) -> Task<void> {
 *         while (auto msg = co_await env.incoming->recv()) {
 *             ChatMessage reply;
 *             reply.set_text("[echo] " + msg->text());
 *             auto r = co_await env.outgoing.send(std::move(reply));
 *             if (!r) break;
 *         }
 *         co_await env.outgoing.close(0, "");
 *     }
 * );
 *
 * // Register with the gRPC router:
 * router.register_bidi("/chat.ChatService/Chat", bidi);
 * @endcode
 *
 * @tparam Req Client → server request message type.
 * @tparam Res Server → client response message type.
 * @param handler_fn Coroutine handler that accepts a `BidiEnvelope<Req, Res>`.
 * @returns Bidi handler callable registerable with the gRPC framework.
 *          Type: `std::function<Task<void>(std::shared_ptr<AsyncChannel<Req>>, Stream<Res>)>`
 */
template <typename Req, typename Res>
std::function<Task<void>(std::shared_ptr<::qbuem::AsyncChannel<Req>>, Stream<Res>)>
make_bidi_handler(
    std::function<Task<void>(BidiEnvelope<Req, Res>)> handler_fn)
{
    return [handler_fn = std::move(handler_fn)](
               std::shared_ptr<::qbuem::AsyncChannel<Req>> incoming,
               Stream<Res>                                  outgoing) -> Task<void> {
        BidiEnvelope<Req, Res> envelope{
            std::move(incoming),
            std::move(outgoing)
        };
        co_await handler_fn(std::move(envelope));
    };
}

} // namespace qbuem::grpc

/** @} */ // end of qbuem_grpc_pipeline
