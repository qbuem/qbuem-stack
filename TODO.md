# qbuem-stack Roadmap

**Zero Latency · Zero Allocation · Zero Dependency**

> **Current Version: v1.5.0** — Zero-dep Security & TLS complete.
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

## 🚀 Current Focus: v1.6.0 — Embedded & PCIe Integration

### v1.6.0 — Embedded & PCIe Integration ([docs/embedded-pcie.md](./docs/embedded-pcie.md))
- [ ] **qbuem::PCIeDevice**: Linux VFIO-based userspace PCIe control.
- [ ] **Interrupt to Reactor Bridge**: Mapping MSI-X interrupts to `eventfd` signals.
- [ ] **UDS Advanced**: FD passing (SCM_RIGHTS) for process-level handle sharing.

### v1.7.0 — High-End Connectivity
- [ ] **RDMA (RoCE)**: Extending zero-copy messaging cross-host via IBVerbs/RoCE.
- [ ] **eBPF Observability**: Standardized cluster-wide tracing via BPF CO-RE.
- [ ] **User-space Storage (SPDK)**: io_uring passthrough for direct NVMe access.

---

## ✅ Completed Milestones

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
