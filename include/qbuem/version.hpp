#pragma once

/**
 * @file qbuem/version.hpp
 * @brief Version constants for the qbuem-stack library.
 * @ingroup qbuem_version
 *
 * This header provides both compile-time version constants and preprocessor
 * macros:
 * - Use `qbuem::Version::major` (and friends) in templates and constexpr code.
 * - Use `QBUEM_VERSION_MAJOR` (and friends) in `#if` preprocessor conditions.
 *
 * ### Version history
 * - 0.1.0: Initial public release.
 * - 0.2.0: AsyncLogger, coroutine Task<T>.
 * - 0.3.0: AsyncConnect awaiter, FixedPoolResource, general API stabilization.
 * - 0.4.0: SO_REUSEPORT per-reactor accept, io_uring SQPOLL, AsyncMiddleware
 *           next(), structured logging, dynamic rate limiting, HTTP Trailer,
 *           enhanced drain mode.
 * - 0.5.0: Reactor::post(), Dispatcher::spawn(), cross-thread wakeup.
 * - 0.6.0: Context, ServiceRegistry, AsyncChannel, Action, StaticPipeline,
 *           TaskGroup, DynamicPipeline, PipelineGraph, MessageBus.
 * - 0.7.0: SocketAddr, TcpListener, TcpStream, UdpSocket, IOSlice, IOVec<N>,
 *           ReadBuf<N>, WriteBuf, BufferPool, zero_copy::, AsyncFile,
 *           IFrameCodec, LengthPrefixedCodec, LineCodec, Http1Codec,
 *           AcceptLoop, ConnectionPool, TimerWheel, PlainTransport.
 * - 0.8.0: RetryPolicy, CircuitBreaker, DeadLetterQueue, TraceContext,
 *           Sampler, SpanExporter, PipelineTracer, kTLS, HugeBufferPool,
 *           MmapArena, SENDMSG_ZC, ACCEPT/RECV_MULTISHOT.
 * - 0.9.0: hot_swap, PriorityChannel, PipelineFactory, SubpipelineAction,
 *           SpscChannel, batch ops, stream operators, DebounceAction,
 *           ThrottleAction, ScatterGatherAction, cpu_hints.
 * - 0.9.1: WindowedAction, SagaOrchestrator, IdempotencyFilter,
 *           CheckpointStore, SloConfig, ErrorBudgetTracker, CanaryRouter.
 * - 0.9.2: NUMA-aware Dispatcher, PerfCounters, eBPF guide, PGO support,
 *           FUTEX_WAIT/WAKE, PipelineVersion, DlqReprocessor.
 * - 1.0.0: Http1Handler, Http2Handler (HPACK), WebSocketHandler,
 *           GrpcHandler<Req,Res>, gRPC <-> Pipeline integration,
 *           TraceMiddleware, InlineRequestBuffer, COMPONENTS support.
 * - 1.1.0: AF_XDP + UMEM, CMake COMPONENTS full support,
 *           reactor forwarding headers, QUIC guide.
 * - 1.2.0: TimerWheel::cancel() O(1), heterogeneous map lookup,
 *           Context::get<T>() inline cache, RadixTree binary search.
 * - 1.3.0: kqueue reactor sophistication (user-space buffer ring,
 *           multi-event batching, pointer-direct dispatch).
 * - 1.4.0: Unified DB abstraction (IDBDriver, ConnectionPool, Statement,
 *           db::Value, SIMD protocol parser),
 *           SHM messaging (SHMChannel, Futex-uring sync, zero-copy DataArena,
 *           unified SHMBus with SHMSource/SHMSink pipeline integration).
 * - 1.5.0: Zero-dependency security & TLS
 *           (kTLS sendfile zero-copy encrypted transmission;
 *            SIMDJwtParser AVX2/SSE4.2/NEON/Scalar dot-scan + Base64url validation;
 *            hardware entropy: RDRAND/RDSEED inline asm with getrandom fallback,
 *            CPUID runtime detection for has_rdrand() / has_rdseed()).
 * - 1.6.0: Embedded & PCIe integration
 *           (PCIeDevice VFIO user-space PCIe control, BarMapping, DmaBuffer;
 *            MSIXReactor: MSI-X -> eventfd -> IReactor bridge, VectorStats;
 *            UDS advanced: SCM_RIGHTS FD passing, PeerCredentials, abstract sockets).
 * - 1.7.0: High-end connectivity
 *           (RDMAContext/RDMAChannel IBVerbs RC QP RDMA Write/Read/Send/Recv;
 *            EBPFTracer CO-RE BPF ringbuf/uprobe/kprobe observability;
 *            NVMeIOContext io_uring IORING_OP_URING_CMD passthrough, DMABuffer).
 * - 2.0.0: Enhancement — lock-free infrastructure & JWT pipeline integration
 *           (LockFreeConnectionPool LIFO FreeStack O(1) lock-free acquire/release;
 *            FutexSync IORING_OP_FUTEX_WAIT/WAKE + syscall fallback;
 *            FutexMutex cross-process RAII mutex; FutexSemaphore counting semaphore;
 *            JwtAuthAction<Msg> SIMD JWT pipeline action with LRU cache + stats).
 * - 2.1.0: Pipeline <-> IPC full integration
 *           (PipelineBuilder::with_source() / with_sink() IPC bridge adapters;
 *            MessageBusSource<T>, MessageBusSink<T>;
 *            SHMSource<T>, SHMSink<T>; SHMChannel::unlink()).
 * - 2.2.0: Monadic HTTP fetch client (curl-free)
 *           (Result::map/and_then/transform_error/value_or monad operations;
 *            ParsedUrl RFC 3986 parser; FetchRequest/FetchResponse builder API;
 *            fetch() JavaScript-style entry point; DnsResolver non-blocking;
 *            FetchRequest::timeout() + max_redirects(); FetchClient connection pool;
 *            TlsStream / fetch_tls() kTLS kernel offload for HTTPS).
 * - 3.3.0: C++23 enforcement + SIMD NEON parity + Zero-Allocation ConfigManager
 *           (std::print/std::println replaces all printf/fprintf/std::cerr;
 *            std::jthread replaces std::thread; std::to_underlying for enum casts;
 *            NEON SIMD parity in websocket XOR masking, erasure coding GF(2^8),
 *            JSON/binary SIMD validator scan + hardware CRC32, HTTP header-end scan,
 *            base64url encoding, constant-time comparison;
 *            ConfigManager: zero-heap fixed-capacity ConfigTable<Cap>,
 *            Secret<T> move-only volatile-wipe with [REDACTED] std::formatter).
 */

/**
 * @defgroup qbuem_version Version
 * @brief Library version identification symbols.
 *
 * Both `constexpr` struct constants and C preprocessor macros are provided
 * so that the version can be queried at compile time and at the preprocessor
 * level. Follows Semantic Versioning 2.0.0 (https://semver.org).
 * @{
 */

#include <string_view>

namespace qbuem {

/**
 * @brief Compile-time version information for qbuem-stack.
 *
 * All members are `constexpr` and may be used in `static_assert` expressions
 * or as template non-type parameters.
 *
 * @note Semantic versioning rules:
 *   - `major`: incremented on backwards-incompatible API changes.
 *   - `minor`: incremented when new features are added in a backwards-compatible manner.
 *   - `patch`: incremented for backwards-compatible bug fixes only.
 *
 * @code
 * static_assert(qbuem::Version::major >= 3, "qbuem-stack 3.x required");
 * std::print("{}\n", qbuem::Version::string); // "3.3.0"
 * @endcode
 */
struct Version {
  /** @brief Major version number. Incremented on backwards-incompatible API changes. */
  static constexpr int major = 3;

  /** @brief Minor version number. Incremented when new backwards-compatible features are added. */
  static constexpr int minor = 3;

  /** @brief Patch version number. Incremented for backwards-compatible bug fixes only. */
  static constexpr int patch = 0;

  /** @brief Version string in "major.minor.patch" format (null-terminated). */
  static constexpr std::string_view string = "3.3.0";
};

} // namespace qbuem

/** @brief Major version number (for use in preprocessor `#if` conditions). */
#define QBUEM_VERSION_MAJOR 3

/** @brief Minor version number (for use in preprocessor `#if` conditions). */
#define QBUEM_VERSION_MINOR 3

/** @brief Patch version number (for use in preprocessor `#if` conditions). */
#define QBUEM_VERSION_PATCH 0

/** @brief Version string literal "major.minor.patch" (for use in preprocessor conditions). */
#define QBUEM_VERSION_STRING "3.3.0"

/** @} */ // end of qbuem_version
