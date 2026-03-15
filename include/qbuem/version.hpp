#pragma once

/**
 * @file qbuem/version.hpp
 * @brief qbuem-stack 라이브러리의 버전 정보를 정의합니다.
 * @ingroup qbuem_version
 *
 * 이 헤더는 컴파일 타임 버전 상수와 전처리기 매크로를 모두 제공합니다.
 * - 템플릿/constexpr 코드에서는 `qbuem::Version::major` 등의 상수를 사용하세요.
 * - 전처리기 조건 분기(`#if`)가 필요한 경우에는 매크로(`QBUEM_VERSION_MAJOR`)를 사용하세요.
 *
 * ### 버전 이력
 * - 0.1.0: 최초 공개 릴리스
 * - 0.2.0: 비동기 로거(AsyncLogger), 코루틴 Task 추가
 * - 0.3.0: AsyncConnect awaiter, FixedPoolResource, 전반적 API 안정화
 * - 0.4.0: SO_REUSEPORT per-reactor accept, io_uring SQPOLL, AsyncMiddleware
 *           next(), Structured logging, 동적 Rate Limit, HTTP Trailer,
 *           Drain mode 강화
 * - 0.5.0: Reactor::post(), Dispatcher::spawn(), cross-thread wakeup
 * - 0.6.0: Context, ServiceRegistry, AsyncChannel, Action, StaticPipeline,
 *           TaskGroup, DynamicPipeline, PipelineGraph, MessageBus
 * - 0.7.0: SocketAddr, TcpListener, TcpStream, UdpSocket, IOSlice, IOVec<N>,
 *           ReadBuf<N>, WriteBuf, BufferPool, zero_copy::, AsyncFile,
 *           IFrameCodec, LengthPrefixedCodec, LineCodec, Http1Codec,
 *           AcceptLoop, ConnectionPool, TimerWheel, PlainTransport
 * - 0.8.0: RetryPolicy, CircuitBreaker, DeadLetterQueue, TraceContext,
 *           Sampler, SpanExporter, PipelineTracer, kTLS, HugeBufferPool,
 *           MmapArena, SENDMSG_ZC, ACCEPT/RECV_MULTISHOT
 * - 0.9.0: hot_swap, PriorityChannel, PipelineFactory, SubpipelineAction,
 *           SpscChannel, batch ops, stream operators, DebounceAction,
 *           ThrottleAction, ScatterGatherAction, cpu_hints
 * - 0.9.1: WindowedAction, SagaOrchestrator, IdempotencyFilter,
 *           CheckpointStore, SloConfig, ErrorBudgetTracker, CanaryRouter
 * - 0.9.2: NUMA-aware Dispatcher, PerfCounters, eBPF guide, PGO support,
 *           FUTEX_WAIT/WAKE, PipelineVersion, DlqReprocessor
 * - 1.0.0: Http1Handler, Http2Handler (HPACK), WebSocketHandler,
 *           GrpcHandler<Req,Res>, gRPC↔Pipeline integration,
 *           TraceMiddleware, InlineRequestBuffer, COMPONENTS support
 * - 1.1.0: AF_XDP + UMEM, cmake COMPONENTS full support,
 *           reactor/* forwarding headers, QUIC guide
 * - 1.2.0: TimerWheel::cancel() O(1), Heterogeneous map lookup,
 *           Context::get<T>() inline cache, RadixTree binary search
 * - 1.3.0: Kqueue reactor sophistication (User-space Buffer Ring,
 *           Multi-event Batching, Pointer-direct Dispatch)
 */

/**
 * @defgroup qbuem_version Version
 * @brief 라이브러리 버전 식별 심볼 모음.
 *
 * 런타임 및 컴파일 타임 양쪽에서 버전을 조회할 수 있도록
 * 구조체 상수(constexpr)와 C 매크로 두 가지 형태로 제공됩니다.
 * Semantic Versioning 2.0.0(https://semver.org)을 따릅니다.
 * @{
 */

#include <string_view>

namespace qbuem {

/**
 * @brief qbuem-stack 라이브러리 버전 정보를 담는 구조체.
 *
 * 모든 멤버는 `constexpr`이므로 컴파일 타임 상수로 사용할 수 있습니다.
 * static_assert나 템플릿 파라미터에서도 활용 가능합니다.
 *
 * @note Semantic Versioning 규칙:
 *   - `major`: 하위 호환이 깨지는 API 변경 시 증가
 *   - `minor`: 하위 호환을 유지하면서 기능 추가 시 증가
 *   - `patch`: 버그 수정만 있을 경우 증가
 *
 * @code
 * static_assert(qbuem::Version::major >= 0, "버전 확인");
 * std::cout << qbuem::Version::string << '\n'; // "1.3.0"
 * @endcode
 */
struct Version {
  /** @brief Major 버전 번호. API 하위 호환이 깨질 때 증가합니다. */
  static constexpr int major = 1;

  /** @brief Minor 버전 번호. 하위 호환을 유지하며 새 기능이 추가될 때 증가합니다. */
  static constexpr int minor = 3;

  /** @brief Patch 버전 번호. 버그 수정만 이루어질 때 증가합니다. */
  static constexpr int patch = 0;

  /** @brief "major.minor.patch" 형식의 버전 문자열 (null-terminated 보장). */
  static constexpr std::string_view string = "1.3.0";
};

} // namespace qbuem

/** @brief Major 버전 번호 (전처리기 조건 분기용). */
#define QBUEM_VERSION_MAJOR 1

/** @brief Minor 버전 번호 (전처리기 조건 분기용). */
#define QBUEM_VERSION_MINOR 3

/** @brief Patch 버전 번호 (전처리기 조건 분기용). */
#define QBUEM_VERSION_PATCH 0

/** @brief "major.minor.patch" 형식의 버전 문자열 리터럴 (전처리기 조건 분기용). */
#define QBUEM_VERSION_STRING "1.3.0"

/** @} */ // end of qbuem_version
