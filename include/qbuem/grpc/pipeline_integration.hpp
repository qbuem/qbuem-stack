#pragma once

/**
 * @file qbuem/grpc/pipeline_integration.hpp
 * @brief gRPC ↔ Pipeline 통합 어댑터
 * @defgroup qbuem_grpc_pipeline gRPC Pipeline Integration
 * @ingroup qbuem_grpc
 *
 * 이 헤더는 gRPC 스트리밍 컨텍스트와 qbuem-stack 파이프라인 채널을
 * 연결하는 어댑터 타입 및 팩토리 함수를 제공합니다.
 *
 * 외부 의존성 없이 표준 라이브러리와 qbuem 내부 타입만 사용합니다.
 *
 * ### 제공 컴포넌트
 * - `Stream<T>`         : 서버 → 클라이언트 단방향 스트리밍 타입 (코루틴 기반)
 * - `BidiEnvelope<Req,Res>` : 양방향 스트리밍 봉투 구조체
 * - `grpc_server_streaming_to_pipeline()` : Server Streaming → Pipeline 어댑터
 * - `grpc_client_streaming_to_channel()`  : Client Streaming → AsyncChannel 어댑터
 * - `make_bidi_handler()`                 : Bidi 핸들러 래퍼 팩토리
 *
 * ### 설계 원칙
 * - 모든 전송 로직은 주입된 함수 객체(`push_fn_`, `close_fn_`)로 위임합니다.
 * - `Stream<T>`는 코루틴 Task를 반환하므로 `co_await`로 백프레셔를 처리합니다.
 * - 파이프라인과의 연결은 `AsyncChannel<T>`를 통한 채널 기반 합성을 사용합니다.
 *
 * ### 사용 예시
 * @code
 * // ── Server Streaming: 파이프라인 결과를 클라이언트에 스트리밍 ──
 * auto handler = grpc_server_streaming_to_pipeline<MyReq, MyRes>(
 *     my_pipeline_handler_fn,  // Task<Result<MyRes>>(MyReq) 시그니처
 *     my_pipeline              // AsyncChannel<MyRes> 공급 파이프라인
 * );
 * // handler 타입: std::function<Task<void>(MyReq, Stream<MyRes>)>
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
#include <qbuem/pipeline/channel.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace qbuem::grpc {

// ─── AsyncChannel 전방 참조 (pipeline/channel.hpp 에서 가져옴) ───────────────
// pipeline/channel.hpp 가 AsyncChannel<T>를 qbuem 네임스페이스에 정의합니다.
// 이 파일에서는 qbuem::AsyncChannel<T>를 사용합니다.

// ─── Stream<T> ───────────────────────────────────────────────────────────────

/**
 * @brief 코루틴 기반 서버 → 클라이언트 단방향 스트리밍 타입.
 *
 * gRPC Server Streaming RPC 또는 Bidirectional Streaming RPC에서
 * 서버가 클라이언트로 데이터를 전송할 때 사용합니다.
 *
 * 내부적으로 두 개의 함수 객체(`push_fn_`, `close_fn_`)에 전송 로직을 위임하므로
 * 실제 전송 레이어(HTTP/2 DATA 프레임, 테스트 목, 인메모리 버퍼 등)와 독립적으로
 * 동작합니다.
 *
 * ### 상태 전이
 * - 생성 시: `open_ == true`.
 * - `close()` 호출 후: `open_ == false`. 이후 `send()`는 에러 반환.
 * - `push_fn_` 반환값이 `false`이면 전송 실패 → `open_` 을 `false`로 전환.
 *
 * ### 코루틴 사용 패턴
 * @code
 * Task<void> handle_stream(MyRequest req, Stream<MyResponse> stream) {
 *     for (int i = 0; i < 10 && stream.is_open(); ++i) {
 *         MyResponse res;
 *         res.set_value(i);
 *         auto r = co_await stream.send(std::move(res));
 *         if (!r) break;  // 클라이언트 연결 끊김
 *     }
 *     co_await stream.close(0, "OK");
 * }
 * @endcode
 *
 * @tparam T 스트림으로 전송할 메시지 타입.
 */
template <typename T>
class Stream {
public:
    /**
     * @brief `push_fn`과 `close_fn`으로 Stream을 구성합니다.
     *
     * @param push_fn  아이템 하나를 클라이언트로 전송하는 함수.
     *                 반환값이 `true`이면 전송 성공, `false`이면 연결 끊김.
     * @param close_fn gRPC 트레일러를 전송하고 스트림을 종료하는 함수.
     *                 첫 번째 인자: gRPC 상태 코드 (0 = OK).
     *                 두 번째 인자: 사람이 읽을 수 있는 메시지 문자열.
     */
    explicit Stream(std::function<bool(T)>                        push_fn,
                    std::function<void(int, std::string)>         close_fn)
        : push_fn_(std::move(push_fn))
        , close_fn_(std::move(close_fn))
        , open_(true)
    {}

    // 복사 비허용 (push_fn_/close_fn_ 의 단일 소유 의미론 유지)
    Stream(const Stream&)            = delete;
    Stream& operator=(const Stream&) = delete;

    // 이동 허용
    Stream(Stream&&) noexcept            = default;
    Stream& operator=(Stream&&) noexcept = default;

    ~Stream() = default;

    // ── Public API ─────────────────────────────────────────────────────────

    /**
     * @brief 아이템 하나를 클라이언트로 전송합니다.
     *
     * `push_fn_`을 호출하여 실제 전송을 수행합니다.
     * 스트림이 닫혀 있거나 전송 실패 시 에러를 반환합니다.
     *
     * @param value 전송할 메시지 값 (이동됨).
     * @returns 전송 성공 시 `Result<void>::ok()`.
     *          스트림이 닫혀 있으면 `errc::broken_pipe`.
     *          `push_fn_` 반환 `false` 시 `errc::connection_reset`.
     *
     * ### 예시
     * @code
     * auto r = co_await stream.send(MyResponse{});
     * if (!r) {
     *     // 클라이언트 연결이 끊겼거나 스트림이 닫힘
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
        co_return Result<void>::ok();
    }

    /**
     * @brief 스트림을 닫고 gRPC 트레일러를 전송합니다.
     *
     * `close_fn_`을 통해 `grpc-status` 및 선택적 `grpc-message` 트레일러를
     * 전송합니다. 이 함수 호출 이후 `is_open() == false`.
     *
     * 이미 닫힌 스트림에서 호출하는 것은 안전하지만 `close_fn_`은 재호출되지
     * 않습니다.
     *
     * @param grpc_status gRPC 상태 코드 (0 = OK, 기타 = 에러 코드).
     * @param message     선택적 사람이 읽을 수 있는 메시지 (기본값: 빈 문자열).
     *
     * ### 예시
     * @code
     * co_await stream.close(0, "");        // 정상 종료
     * co_await stream.close(13, "내부 오류"); // INTERNAL 에러
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
     * @brief 스트림이 아직 열려 있는지 확인합니다.
     *
     * `send()` 실패 또는 `close()` 호출 이후 `false`를 반환합니다.
     *
     * @returns 스트림이 열려 있으면 `true`, 닫혀 있으면 `false`.
     */
    [[nodiscard]] bool is_open() const noexcept { return open_; }

private:
    /** @brief 아이템을 실제 전송 레이어로 밀어 넣는 함수. */
    std::function<bool(T)>                push_fn_;
    /** @brief 스트림을 종료하고 gRPC 트레일러를 전송하는 함수. */
    std::function<void(int, std::string)> close_fn_;
    /** @brief 스트림 열림 상태 플래그. */
    bool open_{true};
};

// ─── BidiEnvelope<Req, Res> ──────────────────────────────────────────────────

/**
 * @brief 양방향 스트리밍 봉투 — 클라이언트 수신 채널과 서버 송신 스트림을 묶습니다.
 *
 * gRPC Bidirectional Streaming RPC 핸들러에 전달되는 단일 진입 구조체입니다.
 *
 * - `incoming` : 클라이언트 → 서버 방향. `co_await incoming->recv()`로 소비합니다.
 *   EOS(채널 닫힘) 시 `std::nullopt` 반환.
 * - `outgoing`  : 서버 → 클라이언트 방향. `co_await outgoing.send(res)`로 전송합니다.
 *
 * ### 사용 예시
 * @code
 * auto handler = make_bidi_handler<ChatMsg, ChatMsg>(
 *     [](BidiEnvelope<ChatMsg, ChatMsg> env) -> Task<void> {
 *         while (true) {
 *             auto item = co_await env.incoming->recv();
 *             if (!item) break;              // 클라이언트 스트림 종료
 *             ChatMsg echo = *item;
 *             echo.set_prefix("[echo] ");
 *             auto r = co_await env.outgoing.send(std::move(echo));
 *             if (!r) break;                 // 전송 실패 (연결 끊김)
 *         }
 *         co_await env.outgoing.close(0, "");
 *     }
 * );
 * @endcode
 *
 * @tparam Req 클라이언트 → 서버 요청 메시지 타입.
 * @tparam Res 서버 → 클라이언트 응답 메시지 타입.
 */
template <typename Req, typename Res>
struct BidiEnvelope {
    /**
     * @brief 클라이언트 → 서버 방향 비동기 수신 채널.
     *
     * `AsyncChannel<Req>::recv()`를 통해 클라이언트 메시지를 순서대로 수신합니다.
     * 채널이 닫히면 EOS(`std::nullopt`)를 반환합니다.
     */
    std::shared_ptr<::qbuem::AsyncChannel<Req>> incoming;

    /**
     * @brief 서버 → 클라이언트 방향 스트리밍 채널.
     *
     * `Stream<Res>::send()`를 통해 클라이언트로 메시지를 전송합니다.
     * 전송 완료 후 `close()`로 gRPC 트레일러를 전송합니다.
     */
    Stream<Res> outgoing;
};

// ─── 어댑터 팩토리 함수들 ──────────────────────────────────────────────────────

/**
 * @brief Server Streaming RPC 핸들러를 파이프라인에 연결하는 어댑터를 생성합니다.
 *
 * `handler_fn`을 호출하여 요청을 파이프라인으로 전달하고,
 * `pipeline` 채널에서 결과를 읽어 `Stream<Res>`를 통해 클라이언트로 스트리밍합니다.
 *
 * 반환된 함수의 시그니처:
 * ```cpp
 * Task<void>(Req request, Stream<Res> stream)
 * ```
 *
 * ### 동작 흐름
 * 1. `handler_fn(request, pipeline)` 호출 — 요청을 파이프라인 채널로 전달.
 * 2. `pipeline.recv()` 루프 — 파이프라인이 생성하는 결과를 하나씩 수신.
 * 3. 각 결과를 `stream.send(result)` 로 클라이언트에 전달.
 * 4. 파이프라인 EOS 또는 스트림 닫힘 시 `stream.close(0, "OK")` 호출.
 *
 * ### 사용 예시
 * @code
 * // 파이프라인: 요청 하나 → 여러 응답
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
 * @tparam Req 요청 메시지 타입.
 * @tparam Res 응답 메시지 타입.
 * @param handler_fn 요청을 파이프라인 채널로 전달하는 코루틴 함수.
 *                   시그니처: `Task<void>(Req, std::shared_ptr<AsyncChannel<Res>>)`.
 * @param pipeline   파이프라인 결과를 수신하는 공유 비동기 채널.
 * @returns `std::function<Task<void>(Req, Stream<Res>)>` 형태의 gRPC 핸들러.
 */
template <typename Req, typename Res>
std::function<Task<void>(Req, Stream<Res>)>
grpc_server_streaming_to_pipeline(
    std::function<Task<void>(Req, std::shared_ptr<::qbuem::AsyncChannel<Res>>)> handler_fn,
    std::shared_ptr<::qbuem::AsyncChannel<Res>>                                  pipeline)
{
    return [handler_fn = std::move(handler_fn),
            pipeline   = std::move(pipeline)](Req request, Stream<Res> stream) -> Task<void> {
        // 요청을 파이프라인으로 전달
        co_await handler_fn(std::move(request), pipeline);

        // 파이프라인에서 결과를 읽어 클라이언트 스트림으로 전달
        while (stream.is_open()) {
            auto item = co_await pipeline->recv();
            if (!item) {
                // EOS: 파이프라인 채널이 닫힘
                break;
            }
            auto r = co_await stream.send(std::move(*item));
            if (!r) {
                // 클라이언트 연결 끊김
                break;
            }
        }

        // 트레일러 전송 (이미 닫힌 경우 no-op)
        co_await stream.close(0, "OK");
    };
}

/**
 * @brief Client Streaming RPC용 `AsyncChannel<Req>`를 생성하고 피더 핸들러를 반환합니다.
 *
 * 클라이언트 스트리밍 RPC에서 수신되는 메시지를 `AsyncChannel<Req>`에 순서대로
 * 전달하는 핸들러 함수를 함께 생성합니다.
 *
 * 반환 타입:
 * ```cpp
 * struct {
 *     std::shared_ptr<AsyncChannel<Req>> channel;   // 소비자가 recv() 호출
 *     std::function<Task<void>(Req)>     feeder;    // 수신 메시지마다 호출
 * };
 * ```
 *
 * ### 사용 예시
 * @code
 * auto [channel, feeder] = grpc_client_streaming_to_channel<MyRequest, MyResponse>();
 *
 * // gRPC 프레임워크가 수신 메시지마다 feeder 호출
 * co_await feeder(incoming_request);
 *
 * // 소비 측 (별도 코루틴):
 * while (auto msg = co_await channel->recv()) {
 *     process(*msg);
 * }
 * @endcode
 *
 * @tparam Req 클라이언트가 전송하는 요청 메시지 타입.
 * @tparam Res 서버가 반환하는 응답 메시지 타입 (채널 타입 태깅용, 실제 사용 안 함).
 * @param capacity 내부 링 버퍼 용량 (기본값: 256).
 * @returns `channel`과 `feeder`를 담는 구조체.
 */
template <typename Req, typename Res = void>
struct ClientStreamingPair {
    /** @brief 소비자가 `recv()`로 메시지를 읽는 채널. */
    std::shared_ptr<::qbuem::AsyncChannel<Req>> channel;
    /**
     * @brief gRPC 프레임워크가 수신 메시지마다 호출하는 피더 함수.
     *        채널이 닫히면 내부적으로 EOS를 전파합니다.
     */
    std::function<Task<void>(Req)>              feeder;
};

/**
 * @brief `AsyncChannel<Req>` + 피더 핸들러 쌍을 생성합니다.
 *
 * @see ClientStreamingPair
 *
 * @tparam Req 클라이언트 요청 메시지 타입.
 * @tparam Res 응답 메시지 타입 (태깅 전용).
 * @param capacity 채널 링 버퍼 용량 (기본값: 256).
 * @returns `ClientStreamingPair<Req, Res>`.
 *
 * ### 사용 예시
 * @code
 * auto pair = grpc_client_streaming_to_channel<UploadChunk, UploadResult>(512);
 *
 * // gRPC 수신 루프에서:
 * while (has_next_frame()) {
 *     auto chunk = decode_chunk(recv_frame());
 *     co_await pair.feeder(std::move(chunk));
 * }
 * pair.channel->close();  // EOS 알림
 *
 * // 처리 코루틴에서:
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
 * @brief Bidirectional Streaming RPC 핸들러를 `BidiEnvelope` 기반으로 래핑합니다.
 *
 * `handler_fn`을 받아 gRPC Bidirectional Streaming RPC 프레임워크가 기대하는
 * `std::function<Task<void>(std::shared_ptr<AsyncChannel<Req>>, Stream<Res>)>` 형태의
 * 호출 가능 객체로 변환합니다.
 *
 * 내부에서 `BidiEnvelope<Req, Res>`를 구성하고 `handler_fn`에 전달합니다.
 *
 * ### 사용 예시
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
 * // gRPC 라우터에 등록:
 * router.register_bidi("/chat.ChatService/Chat", bidi);
 * @endcode
 *
 * @tparam Req 클라이언트 → 서버 요청 메시지 타입.
 * @tparam Res 서버 → 클라이언트 응답 메시지 타입.
 * @param handler_fn `BidiEnvelope<Req, Res>`를 인자로 받는 코루틴 핸들러.
 * @returns gRPC 프레임워크에 등록 가능한 bidi 핸들러 callable.
 *          타입: `std::function<Task<void>(std::shared_ptr<AsyncChannel<Req>>, Stream<Res>)>`
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
