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
 * - 1.2.1: libc++ portability hotfix — CMake propagates -fexperimental-library
 *           for Clang/AppleClang consumers (std::jthread/stop_token/expected are
 *           gated behind it on libc++); dns.hpp uses std::thread (detached) over
 *           std::jthread. Fixes Clang+libc++ builds (Apple clang, clang<=18).
 * - 1.2.2: Apple-libc++ portability — `qbuem::jthread` (compat/jthread.hpp)
 *           replaces every `std::jthread` use. Apple's libc++ (Xcode <= 15.x)
 *           ships <stop_token> but not std::jthread; the drop-in is built on
 *           std::thread + std::stop_source. Verified: identical test results to
 *           native std::jthread (gcc), builds under clang+libc++.
 * - 1.2.3: first-class `qbuem::Thread` component (core/thread.hpp) — owns the
 *           threading primitive (std::thread + std::stop_token, std::jthread
 *           semantics) and replaces every std::jthread / raw std::thread use.
 *           One always-tested code path on all toolchains (no std::jthread
 *           dependency); carries server extras (thread naming, CPU pinning next).
 *           Supersedes the v1.2.2 compat/jthread.hpp shim.
 * - 1.3.0: GenerationPool ergonomics — emplace() (acquire + placement-construct),
 *           destroy() (~T + release), for_each_live() (iterate live slots; pristine
 *           slots start at an odd generation so even==live is unambiguous).
 * - 1.4.0: SpatialGrid<T, W, H, Layers, BucketSize> (buf/spatial_grid.hpp) — the
 *           object-index companion to GridBitset: maps each (x, y, layer) to the
 *           list of objects there (incl. several per cell, which a presence bitset
 *           cannot), with clear()/insert()/for_each_in_radius() for AOI / broad-phase
 *           queries. 0-alloc rebuild (buckets retain capacity).
 * - 1.4.1: HTTP/SSE flush-on-suspend — true server-push streaming. Response gains
 *           set_stream_fd()/flush()/flush_end()/is_streamed()/is_stream_open();
 *           SseStream gains send_async()/close_async(); a WriteAll awaiter
 *           (core/awaiters.hpp) drains a buffer to a non-blocking socket mid-handler.
 *           App::listen injects the socket fd into async-handler Responses and skips
 *           the post-handler send when streamed. Before this, chunk()/SSE buffered
 *           until the handler returned, so a long-lived SSE loop never reached the
 *           client. Also: global SIGPIPE ignore (writes to a closed peer fail with
 *           EPIPE instead of killing the process). Additive + opt-in: buffered
 *           responses are unchanged.
 * Roadmap (planned, not yet tagged): kqueue reactor sophistication; unified DB
 *           abstraction (IDBDriver/ConnectionPool/Statement); SHM messaging
 *           (SHMChannel/SHMBus) — the AOI broadcast path ("darkness = network").
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
 * - 1.0.0: First public stable release. The 0.x–3.x entries above were
 *           pre-release internal iteration; 1.0.0 consolidates them as the
 *           first tagged/published version (SemVer 1.x baseline). Hardened in
 *           the 2026-06 audit: full TSan/ASan/UBSan CI; pipeline/action worker
 *           lifetime UAF fixes; SHM attacker-trust validation; misuse-resistant
 *           AEAD; opt-in QBUEM_ENABLE_NATIVE_CRYPTO (hardware SHA-2/AES);
 *           crypto/SHM/2-thread-SPSC benchmarks.
 * - 1.1.0: High-level WebSocket server (WsServer/WsConnection) — non-blocking
 *           reactor-driven I/O (no blocking read/write on the event loop),
 *           connection registry + rooms + broadcast, per-connection context,
 *           bounded back-pressure send queue, heartbeat ping/pong dead-peer
 *           detection, fragmentation reassembly, strict RFC 6455 framing
 *           (RSV/opcode/mask/control-frame validation), UTF-8 text validation,
 *           and Close handshake. Built for high-concurrency game/realtime
 *           servers atop the existing WebSocketHandler codec
 *           (encode_frame/decode_frame/encode_header/compute_accept_key).
 *           Plus a high-concurrency hardening pass (docs/audit/
 *           2026-06-17_v1.1.0-scale-audit.md): gRPC length-overflow guard;
 *           HTTP/2 Rapid-Reset + DATA/CONTINUATION/HPACK DoS limits; RUDP
 *           reorder-window bound; PBKDF2 iteration cap; SHMBus topic-name UAF
 *           fix; Arena overflow guard; TimerWheel next_expiry O(1); idempotency
 *           store capacity bound; stream-op busy-spin removal; WS slowloris
 *           timeout; TcpStream write_all/read_exact. (Deferred to v1.2: App
 *           async write, SHM futex, lock-free breaker, full HTTP/2-gRPC-DB
 *           server transports — see the audit doc.)
 * - 1.2.0: High-level completeness + contention round. Misuse-resistant crypto
 *           (crypto/secretbox.hpp seal_easy/open_easy random-nonce AEAD +
 *           password_hash/verify_password; crypto/jwt.hpp HS256 sign/verify with
 *           strict alg check; middleware/jwt_verifier.hpp HmacJwtVerifier);
 *           io/buffered_reader.hpp (read_until/read_line); bounded DNS resolver
 *           concurrency; CircuitBreaker lock-free Closed path; App write paths
 *           fixed to not truncate responses on EAGAIN (non-blocking sockets) +
 *           macOS sendfile no longer busy-spins. (Still deferred: awaiters
 *           persistent registration, SHM futex, MessageBus RCU, full HTTP/2/
 *           gRPC/DB server transports — see docs/audit/2026-06-17_*.)
 * - 1.3.0: GenerationPool — emplace / destroy / for_each_live (ABA-safe
 *           generation-tagged object pool for stable-handle entity storage).
 * - 1.4.0: SpatialGrid<T> — uniform-grid object spatial index for AOI /
 *           broad-phase neighbor queries (insert/move/remove, query-in-radius).
 * - 1.4.1: HTTP SSE / chunked flush-on-suspend — true server-push streaming
 *           (a handler can flush bytes mid-coroutine before it returns).
 * - 1.5.0: Precise fixed-timestep ticking.
 *           TickLoop (core/tick_loop.hpp): drift-compensated absolute-deadline
 *           scheduler, deterministic catch-up of missed ticks (bounded by
 *           max_catchup; a f(seed,tick) sim never skips), zero-allocation jitter
 *           + work latency histograms (p50/p99/p99.9) readable cross-thread,
 *           two drive modes (advance(now) for reactor coroutines / manual; and
 *           run_pinned() nanosleep+busy-spin for sub-ms control loops).
 *           TickScheduler (core/tick_scheduler.hpp): high-level multi-rate
 *           systems (every/phase/order), deterministic (seed,tick) splitmix64
 *           RNG, pause / time-scale / single-step + render-interpolation alpha,
 *           per-system work metrics + overrun watchdog. Verified TSan/ASan/UBSan.
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
 * static_assert(qbuem::Version::major >= 1, "qbuem-stack 1.x required");
 * std::print("{}\n", qbuem::Version::string); // "1.5.0"
 * @endcode
 */
struct Version {
  /** @brief Major version number. Incremented on backwards-incompatible API changes. */
  static constexpr int major = 1;

  /** @brief Minor version number. Incremented when new backwards-compatible features are added. */
  static constexpr int minor = 5;

  /** @brief Patch version number. Incremented for backwards-compatible bug fixes only. */
  static constexpr int patch = 0;

  /** @brief Version string in "major.minor.patch" format (null-terminated). */
  static constexpr std::string_view string = "1.5.0";
};

} // namespace qbuem

/** @brief Major version number (for use in preprocessor `#if` conditions). */
#define QBUEM_VERSION_MAJOR 1

/** @brief Minor version number (for use in preprocessor `#if` conditions). */
#define QBUEM_VERSION_MINOR 5

/** @brief Patch version number (for use in preprocessor `#if` conditions). */
#define QBUEM_VERSION_PATCH 0

/** @brief Version string literal "major.minor.patch" (for use in preprocessor conditions). */
#define QBUEM_VERSION_STRING "1.5.0"

/** @} */ // end of qbuem_version
