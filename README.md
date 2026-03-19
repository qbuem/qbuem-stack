# qbuem-stack

**Zero Latency · Zero Allocation · Zero Dependency**

> **Current Version: v2.2.0** — Monadic HTTP Fetch Client (curl-free). Pipeline ↔ MessageBus ↔ SHM fully integrated.
>
> High-performance C++ infrastructure for Web Application Servers (WAS), Inter-Process Communication (IPC), and Data Processing.

---

## Core Principles

| Principle | Implementation Strategy |
| :--- | :--- |
| **Zero Latency** | SIMD HTTP Parser · `io_uring`/`kqueue` Batching · `sendfile(2)` · `kTLS` |
| **Zero Allocation** | Per-request Arena · `FixedPoolResource` for event entries · Lock-free RingBuffers |
| **Zero Dependency** | Kernel-native syscalls only · No external library dependencies in public headers |
| **Shared-Nothing** | Thread-per-core architecture · NUMA-aware pinning · Minimal cross-thread sync |

---

## ⚡ Performance Truth (The "Hard" Targets)

| Metric | Target Value | Verification Method |
| :--- | :--- | :--- |
| **IPC Latency** | < 200ns (P99.9) | `bench_shm_latency` |
| **HTTP Throughput** | > 10M RPS (Native) / > 40M (Bypass) | `wrk2` / `fb-http-bench` |
| **Event Loop Jitter** | < 5μs | `MicroTicker` built-in metrics |
| **Heap Allocation** | **Strictly 0 Bytes** in hot-paths | `valgrind --tool=massif` |
| **Context Switches** | Near Zero (via Batching) | `perf stat -e context-switches` |

---

---

## Documentation Map

- **[Roadmap & Progress](./TODO.md)**: Current status and 40M RPS / 200ns benchmark targets.
- **[C++23 Modernization](./docs/cpp23-upgrade-strategy.md)**: Roadmap for migrating to pure C++23 standard types (`std::expected`, `std::jthread`, `std::print`).
- **[ARM NEON Optimization](./docs/neon-optimization-strategy.md)**: SIMD acceleration strategy for AArch64 (Erasure Coding, Cryptography, Masking).
- **[Strategic Vision](./docs/strategic-evaluation.md)**: Why `qbuem-stack` is a universal platform.
- **[Feature Status & Advancement](./docs/feature-status-advancement.md)**: Detailed audit and competitive analysis against industry leaders.

### 🚀 Engineering Standards — "Extreme" Definition of Done (DoD)

To maintain world-class performance, every PR must satisfy these quantifiable benchmarks:

### 1. Latency & Throughput Targets
- **IPC Latency (SHM)**: P99.9 < 200ns.
- **Reactor Dispatch**: O(1) jump via `udata`/`fixed_files` (No map lookups).
- **HTTP Throughput**: > 10M RPS (Standard) / > 40M RPS (AF_XDP Bypass).

### 2. Zero-Everything Principle
- **Zero Allocation**: 0 bytes allocated in the hot loop (Verified via `valgrind --tool=massif` or `jemalloc` stats).
- **Zero Copy**: Data remains in registered buffers (`io_uring` fixed / RIO registered) from NIC to User-space.
- **Zero Lock**: No `std::mutex` or `std::shared_ptr` in handlers. Use SPSC RingBuffers and `GenerationPool`.

### 3. Hardware Alignment
- **NUMA-Aware**: Memory must be allocated on the same NUMA node as the processing core.
- **L3 Locality**: Hardware interrupts (MSI-X) pinned to reactor cores.
- **SIMD-First**: All parsing and encryption must use AVX-512 / AES-NI / ISA-L.

---

## 🛠 AI-Ready Implementation Map
*For AI agents and developers implementing features from the roadmap:*

| Area | Reference Design | Key Techniques |
| :--- | :--- | :--- |
| **Observability** | [Observability Suite](./docs/observability-suite.md) | OTLP, eBPF memleak, qbuem-inspector |
| **Windows** | [Windows Support](./docs/windows-support.md) | IOCP, Registered I/O (RIO), Named Pipes |
| **Security/TLS** | [Security & TLS](./docs/security-tls.md) | kTLS, Hardware Offload, AES-NI, SIMD JWT |
| **Hardware/PCIe** | [PCIe Optimization](./docs/pcie-optimization-guide.md) | VFIO, P2PDMA, CXL, MSI-X Affinity |
| **Storage / File**| [Storage Optimization](./docs/storage-optimization-guide.md) | `O_DIRECT`, `copy_file_range`, SPDK |
| **Legacy Linux** | [epoll Optimization](./docs/epoll-optimization-guide.md) | `EPOLLET`, `EPOLLONESHOT`, `pwait2` |
| **Network (Linux)** | [Network Optimization](./docs/network-optimization-guide.md) | `io_uring` Fixed Buffers, Multishot, AF_XDP |
| **Network (macOS)** | [kqueue Optimization](./docs/kqueue-optimization-guide.md) | `udata` O(1) Dispatch, Changelist Batching |
| **High-Performance Core** | [Primitives Guide](./docs/high-performance-primitives.md) | Lock-Free Map, GenerationPool, MicroTicker |
| **Ecosystem & WAS** | [Ecosystem Expansion](./docs/ecosystem-expansion.md) | Zero-copy Templates, ReliableCast |

---

## Quick Start

```cpp
#include <qbuem/qbuem_stack.hpp>

int main() {
    qbuem::App app;

    // Direct performance handler
    app.get("/hello", [](const qbuem::Request& req, qbuem::Response& res) {
        res.status(200).body("Zero allocation hello");
    });

    // C++23 Coroutine Async handler
    app.get("/api/v1/user/:id", [](const qbuem::Request& req, qbuem::Response& res)
        -> qbuem::Task<void> {
        auto id = req.param("id");
        res.status(200).body("{\"id\":\"" + std::string(id) + "\"}");
        co_return;
    });

    return app.listen(8080) ? 0 : 1;
}
```

### Pipeline + IPC Integration

```cpp
#include <qbuem/pipeline/static_pipeline.hpp>
#include <qbuem/pipeline/message_bus.hpp>
#include <qbuem/shm/shm_bus.hpp>

using namespace qbuem;
using namespace qbuem::shm;

// SHMChannel → SHMSource → Pipeline → MessageBusSink → MessageBus
auto pipeline = PipelineBuilder<RawOrder, RawOrder>{}
    .with_source(SHMSource<RawOrder>("trading.raw_orders"))  // SHM input
    .add<ParsedOrder>(stage_parse)
    .add<ValidatedOrder>(stage_validate)
    .with_sink(MessageBusSink<ValidatedOrder>(bus, "validated"))  // MessageBus output
    .build();

pipeline.start(dispatcher);
```

See [examples/06-ipc-messaging/ipc_pipeline](./examples/06-ipc-messaging/ipc_pipeline/README.md) for the complete 5-scenario walkthrough.

---

## Layered Architecture (The 9 Levels)

1. **Foundation**: `result`, `arena`, `crypto`
2. **Async Core**: `task`, `reactor`, `dispatcher`
3. **IO Primitives**: `net`, `buf`, `file`, `shm`
4. **Transport**: `transport`, `codec`, `server`
5. **Web/HTTP**: `http`, `http-server`
6. **Pipeline**: `context`, `channel`, `pipeline`
7. **Extensions**: `graph`, `resilience`, `db-abstraction`, `security-core`
8. **Protocols**: `ws`, `http2`, `grpc`, `db-pg`
9. **Umbrella**: `qbuem`

---

## Feature Support Matrix

| Category | Features |
| :--- | :--- |
| **Network** | TCP/UDP/Unix, `SO_REUSEPORT`, `TCP_FASTOPEN`, `SO_BUSY_POLL`, RDMA/RoCE |
| **Performance** | SIMD HTTP/1.1 Parser, SIMD JSON (via qbuem-json), Direct `kqueue`/`io_uring` access |
| **Pipeline** | Static/Dynamic Pipelines, Hot-swap, DAG Graphs, Message Bus, IPC Bridge |
| **IPC Integration** | `SHMSource<T>`, `SHMSink<T>`, `MessageBusSource<T>`, `MessageBusSink<T>` |
| **Reliability** | Retry, CircuitBreaker, DeadLetterQueue, SLO Tracking, FutexSync |
| **Observability** | W3C TraceContext, Span Exporters (OTLP/Jaeger), SLO Metrics, eBPF |
| **Security** | `kTLS` Tx Offload, SIMD JWT Parser, JwtAuthAction, Hardware Entropy |
| **Advanced** | `AF_XDP` Support, Zero-copy `sendfile`/`splice`, PCIe VFIO, NVMe/SPDK |

---

## Performance Benchmarks (v2.1.0)

| Component | Metric | Result |
| :--- | :--- | :--- |
| **Event Dispatch** | Latency | ~6 μs (macOS/kqueue) |
| **AsyncChannel** | Throughput | 44M ops/s (MPMC) |
| **SHMChannel** | IPC Latency | < 150 ns (inter-process) |
| **HTTP Parser** | Throughput | 317 MB/s (SIMD) |
| **Memory Alloc** | `FixedPool` | 4.5 ns / alloc |
| **Router Lookup** | Latency | 112 ns (RadixTree) |
| **MessageBus** | Fan-out | lock-free, O(subscribers) |

---

## Examples

The [`examples/`](./examples/) directory contains **44 programs** organized into 11 categories, each with a detailed README.

| Category | Highlights |
| :--- | :--- |
| [01 Foundation](./examples/01-foundation/) | `hello_world`, `async_timer` — start here |
| [02 Network](./examples/02-network/) | TCP echo, UDP, Unix socket, WebSocket |
| [03 Memory](./examples/03-memory/) | Arena, zero-copy, NUMA + huge pages |
| [04 Codec & Security](./examples/04-codec-security/) | Length-prefix/line codecs, crypto, security middleware |
| [05 Pipeline](./examples/05-pipeline/) | Fan-out, hot-swap, batching, sensor fusion, windowed processing |
| [06 IPC & Messaging](./examples/06-ipc-messaging/) | SHMChannel, **flagship IPC pipeline**, MessageBus, SPSC |
| [07 Resilience](./examples/07-resilience/) | Retry + CircuitBreaker + DLQ, Saga, Canary, Checkpoint, SLO |
| [08 Observability](./examples/08-observability/) | W3C tracing, TimerWheel, TaskGroup structured concurrency |
| [09 Database](./examples/09-database/) | Connection pool, session store, coroutine JSON |
| [10 Hardware](./examples/10-hardware/) | PCIe VFIO, RDMA, eBPF, NVMe, kTLS, kqueue |
| [11 Advanced Apps](./examples/11-advanced-apps/) | Autonomous driving fusion, HFT platform, game server, I/O dashboard |

**Recommended learning path:**
```
hello_world → async_timer → tcp_echo_server → arena → pipeline/fanout
    → ipc_pipeline → resilience → trading_platform
```

---

## Roadmap Highlights

- **v2.2.0 (Current)**: Monadic HTTP fetch client (curl-free), async DNS, timeout, redirect, connection pool, kTLS HTTPS.
- **v2.3.0 (Next)**: HTTP/3 native (quiche bundled), QUIC transport support.
- **v2.4.0**: AF_XDP production examples, eBPF CO-RE enhancements.

---

*qbuem-stack — Built for mechanical sympathy and extreme performance.*
