## ⚡ Extreme Performance Standards (The Baseline)

All modules must adhere to these quantitative benchmarks to be considered part of the `qbuem` elite stack.

| Metric | Target | Industry Benchmark (Software-only) |
| :--- | :--- | :--- |
| **IPC Latency (SHM)** | **< 200ns** | Aeron ($5\mu s$), Disruptor ($<100ns$ intra-thread) |
| **HTTP Throughput** | **> 40M RPS** | TechEmpower Elite ($7M$), AF_XDP Records ($20\sim40M$) |
| **Network P99.9** | **< 10μs** | High-end HFT Software ($1\sim2\mu s$ tick-to-trade) |
| **Coroutine Switch** | **< 50ns** | Common Boost.Context ($50\sim100ns$) |
| **Jitter (Tick)** | **< 1μs** | Real-time Linux (PREEMPT_RT) target range |
| **Zero Allocation** | **0 Bytes** | Mandatory for safety-critical/HFT systems |
| **SIMD Parsing** | **> 4GB/s** | `simdjson` peak performance range |

> [!IMPORTANT]
> **Experimental Target**: While **10M RPS** is the entry-level for our elite stack, we aim for **20M~40M RPS** on 100GbE using `AF_XDP` bypass to define the true experimental frontier.

> [!NOTE]
> Our targets are set at the **absolute software frontier**. Achieving these requires hardware-level optimization (Mechanical Sympathy) including NUMA pinning, HugePages, and Kernel Bypass (io_uring/XDP).

---

## ✅ Completed: v2.3.0 — HTTP Client Full Feature Set

## ✅ Completed: v2.3.0 — HTTP Client Full Feature Set

> **Goal**: Complete curl-free HTTP client — async DNS, timeout, redirects, connection pool, kTLS HTTPS.

### 1. Language Policy
- [x] **English-only**: All code comments, docs, and user-facing strings must be in English. Rule added to `CLAUDE.md`.

### 2. Async DNS Resolution (`include/qbuem/net/dns.hpp`)
- [x] **`DnsResolver::resolve(host, port)`**: Non-blocking hostname → `SocketAddr` via detached thread + `Reactor::post()`.
- [x] **IP literal fast-path**: `inet_pton` check in `await_ready()` — no thread spawned for numeric IPs.
- [x] **IPv4 preference**: Prefers IPv4 results; falls back to IPv6 if no IPv4 record exists.
- [x] **Reactor integration**: Coroutine resume posted to the originating reactor thread (thread-safe).

### 3. Timeout Support (`include/qbuem/http/fetch.hpp`)
- [x] **`FetchRequest::timeout(duration)`**: Reactor `register_timer()` fires and requests stop via combined stop_token.
- [x] **Combined stop_token**: `CombinedStop` struct combines caller's stop_token with timeout stop_source.
- [x] **Zero overhead when not set**: Timeout path is opt-in; no timer registered when `timeout_ms_ == 0`.

### 4. Automatic Redirect Following (`include/qbuem/http/fetch.hpp`)
- [x] **`FetchRequest::max_redirects(n)`**: Follow 3xx responses up to N hops.
- [x] **303 → GET**: Automatically downgrades method to GET on `303 See Other`.
- [x] **Location header**: Uses the `Location` response header as the next URL.

### 5. DNS Integration in `fetch()` (`include/qbuem/http/fetch.hpp`)
- [x] **Hostname resolution**: `send()` calls `DnsResolver::resolve()` — real hostname support (not just IP literals).
- [x] **All comments English**: Entire `fetch.hpp` rewritten in English.

### 6. FetchClient — Connection Pool + Keep-Alive (`include/qbuem/http/fetch_client.hpp`)
- [x] **`FetchClient`**: Per-host TCP connection pool. `acquire()` / `release()` free-list.
- [x] **Keep-Alive**: Sends `Connection: keep-alive`; returns sockets to pool on server keep-alive response.
- [x] **Stale connection retry**: If a reused connection fails on write, retries with a fresh connection.
- [x] **`set_max_idle_per_host(n)`**, **`set_timeout(d)`**, **`set_max_redirects(n)`** configuration.
- [x] **`idle_count()`**, **`clear_pool()`** pool management.
- [x] **Redirect + monadic chaining**: Full feature parity with `FetchRequest`.

### 7. kTLS HTTPS (`include/qbuem/http/fetch_tls.hpp`)
- [x] **`TlsStream`**: Wraps `TcpStream` + `io::enable_ktls()` activation. Graceful fallback on unsupported kernels.
- [x] **`TlsSessionParams`**: Carries fd + TX/RX `KtlsSessionParams` from the caller's TLS handshake.
- [x] **`TlsFetchRequest`**: HTTP/1.1 request builder over a kTLS-activated stream.
- [x] **`fetch_tls(url, session)`**: Entry point. Caller provides post-handshake session; kernel handles AES-GCM.
- [x] **Zero TLS library dependency**: kTLS offload path requires no OpenSSL/BoringSSL at I/O time.

### 8. Example (`examples/02-network/http_fetch/`)
- [x] **`http_fetch_example.cpp`**: 8 patterns: GET, POST, monadic chain, error, timeout, redirect, URL parser, FetchClient.
- [x] **All English**: Comments and output strings translated to English.

---

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

## ✅ Completed: v2.2.0 — v2.3.0 — Monadic HTTP Client (Full)

### v2.2.0 — curl-free Monadic Fetch
- [x] **Result::map/and_then/transform_error/value_or**: Functor/Monad operations on `Result<T>`. (`common.hpp`)
- [x] **ParsedUrl**: RFC 3986 URL parser (scheme/host/port/path, IPv6 literal support).
- [x] **FetchResponse**: Client HTTP response value type (status, headers, body, ok()).
- [x] **FetchRequest**: Builder-pattern HTTP client (method/header/body/get/post/put/del/patch).
- [x] **fetch()**: JavaScript-like factory entry point.
- [x] **Zero-dependency HTTP/1.1**: Built directly on `TcpStream`. No curl. (`include/qbuem/http/fetch.hpp`)
- [x] **http_fetch_example.cpp**: 8-pattern example file. (`examples/02-network/http_fetch/`)

### v2.3.0 — Full Feature Set (async DNS, timeout, redirect, pool, kTLS)
- [x] **DnsResolver**: Non-blocking `getaddrinfo` via thread + `Reactor::post()`. (`include/qbuem/net/dns.hpp`)
- [x] **Timeout**: `FetchRequest::timeout(d)` — reactor timer + combined stop_token.
- [x] **Redirect**: `FetchRequest::max_redirects(n)` — automatic 3xx follow with 303→GET downgrade.
- [x] **FetchClient**: Per-host connection pool, Keep-Alive, stale-connection retry. (`include/qbuem/http/fetch_client.hpp`)
- [x] **TlsStream / fetch_tls()**: kTLS kernel offload for HTTPS. Caller provides TLS handshake + session keys. (`include/qbuem/http/fetch_tls.hpp`)
- [x] **English-only**: All code comments and docs translated to English. Rule added to `CLAUDE.md`.

---

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

## ✅ Completed: v2.4.0 — High-Performance Essential Primitives
> **Reference Design**: [docs/high-performance-primitives.md](./docs/high-performance-primitives.md) / [docs/storage-optimization-guide.md](./docs/storage-optimization-guide.md) / [docs/epoll-optimization-guide.md](./docs/epoll-optimization-guide.md) / [docs/pcie-optimization-guide.md](./docs/pcie-optimization-guide.md)

- [x] **`LockFreeHashMap<K, V>`**: MPMC non-blocking hash map with atomic CAS per slot. (`include/qbuem/buf/lock_free_hash_map.hpp`)
- [x] **`GenerationPool<T>`**: Lock-free object pool with generation-indexed `Handle<T>` (ABA-safe). (`include/qbuem/buf/generation_pool.hpp`)
- [x] **`IntrusiveList<T>`**: Zero-allocation linked list (objects carry pointers). (`include/qbuem/buf/intrusive_list.hpp`)
- [x] **`MicroTicker`**: High-precision Reactor driver — nanosleep + busy-spin hybrid, <5µs jitter, drift compensation. (`include/qbuem/reactor/micro_ticker.hpp`)
- [x] **`FileIO (O_DIRECT)`**: `DirectFile`, `FileSink<T>`, `FileSource<T>` — page-cache bypass with 512-byte aligned I/O. (`include/qbuem/io/direct_file.hpp`)
- [x] **`Epoll (Edge-Triggered)`**: `EpollReactor` updated to `EPOLLET | EPOLLONESHOT` with automatic post-callback re-arming. (`src/core/epoll_reactor.cpp`)
- [x] **`PCIeDevice (VFIO)`**: Full VFIO implementation — `open()`, `map_bar()`, `alloc_dma_buffer()`, MMIO read/write. (`src/pcie/pcie_device.cpp`)
- [x] **C++23 Migration**: `CMAKE_CXX_STANDARD 23`, `std::unexpected` (via `<expected>`) replaces custom type, `std::println` from `<print>` in examples.

---

## 🏗 Milestone: v2.5.0 — High-Performance Stream processing (Pipeline+)
> **Reference Design**: [docs/ecosystem-expansion.md](./docs/ecosystem-expansion.md)

- [ ] **`StatefulWindow`**: Native support for sliding/tumbling window aggregations.
- [ ] **`DynamicRouter`**: SIMD-accelerated branch predicate evaluation.
- [ ] **`BackpressureMonitor`**: Real-time stage pressure and latency metrics via atomics.

---

## 🏗 Milestone: v2.6.0 — Advanced WAS & Middleware
> **Reference Design**: [docs/ecosystem-expansion.md](./docs/ecosystem-expansion.md)

- [ ] **`qbuem-template`**: Zero-copy pre-compiled template engine.
- [ ] **`ReliableCast<T>`**: Zero-copy 1:N multicast for SHM-based IPC.
- [ ] **`SIMDValidator`**: Wire-speed JSON Schema/Binary validation using SIMD skip-patterns.

---

## 🏗 Milestone: v2.7.0 — Next-Gen Networking (Fetch+)
> **Reference Design**: [docs/fetch-advancement.md](./docs/fetch-advancement.md) / [docs/network-optimization-guide.md](./docs/network-optimization-guide.md)

- [ ] **`HTTP/2 & HTTP/3`**: Frame-based multiplexing and UDP-based low-latency transport.
- [ ] **`Kernel TLS (kTLS)`**: Zero-copy HTTPS integration via kernel-level crypto.
- [ ] **`Zero-Copy Streaming`**: Processing multi-GB payloads using constant memory via body-bound pipelines.

---

## 🏗 Milestone: v2.8.0 — Low-Latency UDP Infrastructure (UDP+)
> **Reference Design**: [docs/udp-architecture.md](./docs/udp-architecture.md) / [docs/network-optimization-guide.md](./docs/network-optimization-guide.md)

- [ ] **`MMSG Batching`**: `recvmmsg` and `sendmmsg` integration for high-throughput datagram processing.
- [ ] **`Reliable UDP (RUDP)`**: Lightweight reliability layer (Sequencing, ACKs) for real-time sync.
- [ ] **`Native Multicast`**: High-speed 1:N distribution for financial and media feeds.

---

## 🌌 Future Vision: v3.0.0 — The Ultimate Protocol Stack
> **Reference Design**: [docs/feature-status-advancement.md](./docs/feature-status-advancement.md)

- [ ] **`AF_XDP Bypass`**: Direct user-space networking for sub-microsecond packet processing.
- [ ] **`Distributed Storage (NVMe-oF)`**: High-speed remote block access via RDMA/TCP.
- [ ] **`SIMD Erasure Coding`**: Wire-speed data redundancy using AVX-512/ISA-L.
- [ ] **`Distributed Pipelines`**: Stretching pipelines across hosts via RDMA/InfinityBand.
- [ ] **`Post-Quantum Security`**: Native C++23 Kyber/Dilithium support for v3 identities.
- [ ] **`Smart DB Cache`**: SHM-shared and hardware-invalidated query caches.

---
 
 ## 🏗 Milestone: v3.1.0 — Observability & Visual Tooling
 > **Reference Design**: [docs/observability-suite.md](./docs/observability-suite.md)
 
 - [ ] **`qbuem-tracer`**: Zero-allocation OTLP/SHM tracer with `start_lifecycle()` API.
- [ ] **`qbuem-logger`**: Trace-aware async logger with lifecycle correlation.
- [ ] **`qbuem-cli`**: Dual-mode CLI (TUI dashboard + HTML-serve backend).
 - [ ] **`qbuem-inspector`**: Visual dashboard with "Full Journey" timeline view.
 - [ ] **`eBPF Bridge`**: Production-grade memory leak & perf analysis integration.
 - [ ] **`CoroExplorer`**: Coroutine stack-trace and suspension point visualization.
 
 ---
 
 ## 🌌 Milestone: v3.2.0 — Elite Tooling & Chaos Engineering
 > **Reference Design**: [docs/observability-suite.md](./docs/observability-suite.md)
 
 - [ ] **`Affinity Inspector`**: Real-time Core/NUMA mapping and topology visualization.
 - [ ] **`Buffer Heatmap`**: Visual lifecycle tracking for zero-copy memory segments.
 - [ ] **`Chaos-Hardware`**: User-space fault-injection for PCIe/NVMe/RDMA.
 - [ ] **`Traffic-Twin`**: Deterministic protocol recording and replay tool.
 
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
