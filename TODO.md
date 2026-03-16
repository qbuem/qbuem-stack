# qbuem-stack Roadmap

**Zero Latency · Zero Allocation · Zero Dependency**

> **Current Version: v2.1.0** — Pipeline ↔ MessageBus ↔ SHM 완전 연계 완료.
>
> High-performance C++ infrastructure for Web, Messaging, and Data Pipelines.

---

## 🏗 Project Vision & Design
- **Single System Image**: Unified I/O for WAS, IPC, and Stream Processing.
- **Kernel-Native**: Directly leveraging `io_uring`, `kqueue`, `kTLS`, and `AF_XDP`.
- **Mechanical Sympathy**: NUMA-aware, cache-aligned, and lock-free by default.

**Documentation:**
- [Pipeline Master Guide](./docs/pipeline-master-guide.md) | [IO Architecture](./docs/io-architecture.md) | [Library Strategy](./docs/library-strategy.md)
- [DB Abstraction](./docs/db-abstraction.md) | [Strategic Vision](./docs/strategic-evaluation.md) | [Windows Support](./docs/windows-support.md) | [Versatility](./docs/versatility-guide.md)

---

## ✅ Completed: v1.4.0 — Unified DB & SHM Messaging
> **Goal**: Achieve sub-microsecond IPC and zero-allocation database access.

### 1. Unified Database Abstraction ([docs/db-abstraction.md](./docs/db-abstraction.md))
- [x] **IDBDriver Interface**: O(1) connection handover and stateless protocol parsing. (`include/qbuem/db/driver.hpp`)
- [x] **ConnectionPool & Statement**: Zero-allocation query preparation and result streaming. (`include/qbuem/db/driver.hpp`)
- [x] **SIMD Protocol Parser**: AVX-512/NEON accelerated parsing for PostgreSQL/MySQL results. (`include/qbuem/db/simd_parser.hpp`)
- [x] **db::Value**: Heap-free variant for parameter binding and result extraction. (`include/qbuem/db/value.hpp`)

### 2. SHM Messaging Infrastructure ([docs/shm-messaging.md](./docs/shm-messaging.md))
- [x] **SHMChannel**: 150ns latency cross-process messaging via `memfd_create`. (`include/qbuem/shm/shm_channel.hpp`)
- [x] **Futex-uring Sync**: `IORING_OP_FUTEX` based wait/wake for lowest possible IPC jitter. (`include/qbuem/shm/shm_channel.hpp`)
- [x] **Zero-copy Data Arena**: Direct memory access to shared segments without copies. (`include/qbuem/shm/shm_channel.hpp`)
- [x] **Unified Message Bus**: Bridging intra-thread `AsyncChannel` and inter-process `SHMChannel`. (`include/qbuem/shm/shm_bus.hpp`)

---

## ✅ Completed: v1.5.0 — Zero-dep Security & TLS

### v1.5.0 — Zero-dep Security & TLS ([docs/security-tls.md](./docs/security-tls.md))
- [x] **kTLS Optimization**: `sendfile` + kTLS for zero-copy encrypted transmission. (`include/qbuem/io/ktls.hpp`)
- [x] **SIMD Auth Parser**: High-speed JWT and token parsing using SIMD predicates. (`include/qbuem/security/simd_jwt.hpp`)
- [x] **Hardware Entropy**: Direct integration with CPU `RDRAND`/`RDSEED` and kernel `getrandom`. (`include/qbuem/crypto.hpp`)

## ✅ Completed: v1.6.0 — Embedded & PCIe Integration

### v1.6.0 — Embedded & PCIe Integration ([docs/embedded-pcie.md](./docs/embedded-pcie.md))
- [x] **qbuem::PCIeDevice**: Linux VFIO-based userspace PCIe control. (`include/qbuem/pcie/pcie_device.hpp`)
- [x] **Interrupt to Reactor Bridge**: Mapping MSI-X interrupts to `eventfd` signals. (`include/qbuem/pcie/msix_reactor.hpp`)
- [x] **UDS Advanced**: FD passing (SCM_RIGHTS) for process-level handle sharing. (`include/qbuem/net/uds_advanced.hpp`)

## ✅ Completed: v1.7.0 — High-End Connectivity

### v1.7.0 — High-End Connectivity
- [x] **RDMA (RoCE)**: Extending zero-copy messaging cross-host via IBVerbs/RoCE. (`include/qbuem/rdma/rdma_channel.hpp`)
- [x] **eBPF Observability**: Standardized cluster-wide tracing via BPF CO-RE. (`include/qbuem/ebpf/ebpf_tracer.hpp`)
- [x] **User-space Storage (SPDK)**: io_uring passthrough for direct NVMe access. (`include/qbuem/spdk/nvme_io.hpp`)

## ✅ Completed: v2.1.0 — Pipeline ↔ IPC 완전 연계

### v2.1.0 — Pipeline IPC Bridge & Reliability

- [x] **PipelineBuilder::with_source()**: 외부 소스(SHMSource, MessageBusSource)를 Pipeline Head에 연결. 소스 펌프 코루틴 자동 생성. (`include/qbuem/pipeline/static_pipeline.hpp`)
- [x] **PipelineBuilder::with_sink()**: 외부 싱크(SHMSink, MessageBusSink)를 Pipeline Tail에 연결. drain 코루틴 자동 생성. (`include/qbuem/pipeline/static_pipeline.hpp`)
- [x] **MessageBusSource\<T\>**: MessageBus 토픽 구독 → Pipeline 소스 브릿지. `init()+next()` 프로토콜. (`include/qbuem/pipeline/message_bus.hpp`)
- [x] **MessageBusSink\<T\>**: Pipeline Tail → MessageBus 발행 브릿지. `init()+sink()` 프로토콜. (`include/qbuem/pipeline/message_bus.hpp`)
- [x] **SHMSource\<T\>**: `SHMChannel<T>` Consumer 측 Pipeline 어댑터. `name_`을 `std::string`으로 소유 (dangling ref 방지). (`include/qbuem/shm/shm_bus.hpp`)
- [x] **SHMSink\<T\>**: `SHMChannel<T>` Producer 측 Pipeline 어댑터. (`include/qbuem/shm/shm_bus.hpp`)
- [x] **SHMChannel\<T\>::unlink()**: SHM 세그먼트 파일시스템 이름 제거 (멱등, ENOENT → ok). (`include/qbuem/shm/shm_channel.hpp`)
- [x] **ipc_pipeline_example.cpp**: 5개 시나리오 복합 통합 예시 (SHM→Pipeline, Pipeline→MessageBus, 전체 체인). (`examples/ipc_pipeline_example.cpp`)
- [x] **pipeline_ipc_test.cpp**: Pipeline ↔ MessageBus IPC 통합 테스트 7개. (`tests/pipeline_ipc_test.cpp`)

---

## ✅ Completed: v2.0.0 — 고도화 (Enhancement)

### v2.0.0 — Lock-free Infrastructure & JWT Pipeline Integration
- [x] **LockFreeConnectionPool**: LIFO FreeStack CAS-based O(1) lock-free acquire/release + PooledConnection RAII guard. (`include/qbuem/db/connection_pool.hpp`)
- [x] **FutexSync**: `IORING_OP_FUTEX_WAIT/WAKE` non-blocking coroutine futex + syscall fallback. (`include/qbuem/shm/futex_sync.hpp`)
- [x] **FutexMutex**: Cross-process RAII mutex (states: 0=Unlocked/1=Locked/2=Waiters). (`include/qbuem/shm/futex_sync.hpp`)
- [x] **FutexSemaphore**: Cross-process counting semaphore. (`include/qbuem/shm/futex_sync.hpp`)
- [x] **JwtAuthAction**: SIMD JWT Pipeline Action with LRU cache, Stats atomics, zero-copy JwtClaims Context injection. (`include/qbuem/security/jwt_action.hpp`)

---

## ✅ Completed Milestones

<details>
<summary><b>v2.0.0 — 고도화 (Enhancement)</b></summary>

- [x] **LockFreeConnectionPool**: LIFO FreeStack (CAS push/pop, max 256 slots), O(1) lock-free `acquire()`/`release()`, waiter queue, idle timeout cleanup, `warmup()`, `drain()`.
- [x] **PooledConnection**: RAII guard for pool-acquired connections. `acquire(pool)` factory. Move-only.
- [x] **FutexWord**: 4-byte `alignas(4) atomic<uint32_t>` for cross-process futex. Static asserts for size/alignment.
- [x] **FutexSync**: `IORING_OP_FUTEX_WAIT` SQE + syscall fallback. `wait()`, `wake()`, `wake_all()`, `has_uring_futex()`.
- [x] **FutexMutex**: Cross-process RAII mutex (0=Unlocked, 1=Locked, 2=Locked+Waiters). `lock()`, `try_lock()`, `unlock()`, `LockGuard`.
- [x] **FutexSemaphore**: Cross-process counting semaphore. `acquire()`, `try_acquire()`, `release()`, `value()`.
- [x] **JwtClaims**: Zero-copy `string_view` fields (sub/iss/aud) + int64 exp/iat/nbf. `is_valid_at()` with leeway.
- [x] **JwtAuthConfig**: leeway_sec, cache_size=256, require_exp, require_sub, auth_header, kBearerPrefixLen=7.
- [x] **JwtAuthResult**: OK/NoToken/InvalidFormat/Expired/NotYetValid/SignatureInvalid/MissingClaim/CacheHit.
- [x] **JwtAuthAction\<Msg\>**: Token extraction (duck typing), LRU cache (FNV-1a direct-mapped), SIMD parse, exp check, ITokenVerifier delegation, Context::put\<JwtClaims\>(), Stats atomics.
</details>

<details>
<summary><b>v1.7.0 — High-End Connectivity</b></summary>

- [x] **RDMAContext**: `open(dev_name)` IBVerbs device + PD allocation. Move-only.
- [x] **RDMAChannel**: RC QP setup, `local_info()`/`connect()`, `write()`/`read()`/`send()`/`post_recv()`, `poll_cq()`. In-flight limiting.
- [x] **QPInfo**: qp_num, lid, gid[16], psn for QP handshake.
- [x] **Completion**: wr_id, bytes, status, opcode. `ok()` check.
- [x] **EBPFTracer**: CO-RE BPF load, `enable()`/`disable()`, BPF ringbuf `poll()`, `subscribe()` callback, `read_stats()`, `reset_stats()`, `lookup_map<K,V>()`.
- [x] **TraceEvent**: 64-byte cache-line aligned struct, `set_label()`/`get_label()`.
- [x] **EventType**: TcpAccept/TcpClose/HttpParse/PipelineAction/IoUring/ShmChannel/RDMA/JwtVerify/Custom.
- [x] **qbuem_trace_point**: `[[gnu::noinline]]` no-op uprobe attachment point. `QBUEM_TRACE` macro.
- [x] **NVMeIOContext**: `open()`, `alloc_dma()`, `read()`/`write()`/`flush()`/`trim()`, `read_scatter()`, `identify_controller()`, `get_log_page()`.
- [x] **DMABuffer**: Abstract base for `mmap(MAP_HUGETLB)` / `posix_memalign` buffers.
- [x] **NVMeResult**: status, `ok()`, `sct()`, `sc()`.
- [x] **NVMeDeviceInfo**: ns_size, lba_size, `total_bytes()`.
- [x] **NVMeStats**: Atomic read_ops/write_ops/read_bytes/write_bytes/errors/avg_latency_ns.
</details>

<details>
<summary><b>v1.6.0 — Embedded & PCIe Integration</b></summary>

- [x] **BDF**: bus/device/function struct + `to_string()`.
- [x] **BarMapping**: vaddr, size, bar_idx. `read32()`/`write32()` MMIO helpers.
- [x] **DmaBuffer**: vaddr, iova, size, container_fd for IOMMU-mapped DMA.
- [x] **PCIeDevice**: `open(bdf)` VFIO, `map_bar()`, `read_mmio32()`/`write_mmio32()`, `alloc_dma_buffer()`. Move-only.
- [x] **VectorStats**: `alignas(64)` irq_count/missed/latency_ns atomics.
- [x] **MSIXReactor**: `setup()`, `wait()` co_await eventfd, `try_consume()`, `mask()`/`unmask()`. Move-only.
- [x] **PeerCredentials**: pid/uid/gid from `SO_PEERCRED`.
- [x] **RecvFdsResult**: fd_count, data_bytes.
- [x] **send_fds()**: `SCM_RIGHTS` FD passing via `sendmsg`.
- [x] **recv_fds()**: `SCM_RIGHTS` FD receiving via `recvmsg`.
- [x] **get_peer_credentials()**: `SO_PEERCRED` getsockopt.
- [x] **bind_abstract()** / **connect_abstract()**: Linux abstract namespace Unix domain sockets.
</details>

<details>
<summary><b>v1.5.0 — Zero-dep Security & TLS</b></summary>

- [x] **kTLS Optimization**: `ktls_sendfile()` / `ktls_sendfile_all()` — sendfile + kTLS zero-copy 암호화 전송. Linux 폴백 EAGAIN 처리 포함.
- [x] **SIMD Auth Parser**: `SIMDJwtParser` — AVX2/SSE4.2/NEON/Scalar `.` 탐색, Base64url 검증, 클레임 추출 (zero-allocation). JwtView 클레임 API (`claim()`, `claim_int()`, `is_expired()`).
- [x] **Hardware Entropy**: `rdrand64()`, `rdseed64()` CPUID 기반 inline asm, `hw_entropy_fill()`, `hw_seed_fill()`, `has_rdrand()`, `has_rdseed()`. getrandom/arc4random 투명 폴백.
</details>

<details>
<summary><b>v1.4.0 — Unified DB & SHM Messaging</b></summary>

- [x] **IDBDriver Interface**: O(1) connection handover and stateless protocol parsing.
- [x] **ConnectionPool & Statement**: Zero-allocation query preparation and result streaming.
- [x] **SIMD Protocol Parser**: AVX-512/NEON accelerated parsing for PostgreSQL/MySQL results.
- [x] **db::Value**: Heap-free variant (≤32B) for parameter binding and result extraction.
- [x] **SHMChannel**: 150ns latency cross-process messaging via `memfd_create` / `shm_open`.
- [x] **Futex-uring Sync**: `IORING_OP_FUTEX` based wait/wake for lowest possible IPC jitter.
- [x] **Zero-copy Data Arena**: Direct memory access to shared segments without copies.
- [x] **Unified SHMBus**: Bridging intra-thread `AsyncChannel` and inter-process `SHMChannel`, with `SHMSource`/`SHMSink` Pipeline integration.
</details>

<details>
<summary><b>v1.3.0 — Kqueue Sophistication (macOS)</b></summary>

- [x] **User-space Buffer Ring**: High-performance buffer selection mimicking `io_uring`.
- [x] **Multi-event Batching**: `kevent` changelist optimization to minimize syscalls.
- [x] **Pointer-direct Dispatch**: `udata`-based direct callback invocation.
- [x] **Principle Alignment**: Integrated `TimerWheel` and `FixedPoolResource`.
</details>

<details>
<summary><b>v1.2.0 — Performance Refinement</b></summary>

- [x] **O(1) Timer Wheel**: 4-level hierarchical wheel for low-overhead timers.
- [x] **Zero-allocation HTTP Params**: Heterogeneous map lookup for header/query params.
- [x] **Context Cache**: Inline cache for `Context::get<T>()` lookups.
- [x] **RadixTree Binary Search**: Faster child node traversal for long routes.
</details>

<details>
<summary><b>v1.0.0 — Protocol Handlers & Pipelines</b></summary>

- [x] **Static & Dynamic Pipelines**: Compiled type-chains and runtime configurable graphs.
- [x] **HTTP/1.1 Handler**: Full keep-alive, chunked, and pipeline support.
- [x] **WebSocket Handler**: RFC 6455 implementation with zero-alloc masking.
- [x] **Message Bus**: Unary, Streaming, and Bidi-stream support.
</details>

<details>
<summary><b>v0.x — Core Foundation</b></summary>

- [x] `IReactor` abstraction (epoll, kqueue, io_uring).
- [x] `Task<T>` Coroutines (symmetric transfer).
- [x] `Arena` & `FixedPoolResource` allocators.
- [x] SIMD HTTP Parser.
- [x] Core Middleware (CORS, RateLimit, Security).
</details>

---

## 🛠 Technical Reference: 9-Level Strategy ([docs/library-strategy.md](./docs/library-strategy.md))

| Level | Component | Responsibility |
| :--- | :--- | :--- |
| **0** | `result`, `arena`, `crypto` | Memory & Error Handling (Foundation) |
| **1** | `task`, `reactor`, `dispatcher` | Async Core (Event Loop & Coroutines) |
| **2** | `epoll`, `kqueue`, `iouring` | OS-specific Reactor Implementations |
| **3** | `net`, `buf`, `shm`, `file` | IO & Networking Primitives |
| **4** | `transport`, `codec`, `server` | Framing & Session Management |
| **5** | `http`, `http-server` | Web Application Layer |
| **6** | `context`, `channel`, `pipeline` | Stream Processing Core |
| **7** | `graph`, `resilience`, `db-core` | Advanced Orchestration & Reliability |
| **8** | `ws`, `http2`, `grpc`, `db-pg` | Specialized Protocol Handlers |
| **9** | `qbuem` | Umbrella Library |

---

## 📦 Dependency Principles
- **No External Multi-purpose Libraries**: No Boost, no OpenSSL in headers, no libc++ extensions.
- **Kernel-Aligned**: We implement protocols directly over kernel primitives (e.g., kTLS, SHM).
- **Pay-for-what-you-use**: Modular CMake components allow linking only the necessary layers.
