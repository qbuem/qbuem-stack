# qbuem-stack

**Zero Latency · Zero Allocation · Zero Dependency**

> **Current Version: v1.3.0** — 651/651 Tests Passed. 
> 
> High-performance C++ infrastructure for Web Application Servers (WAS), Inter-Process Communication (IPC), and Data Processing.

---

## 🏗 Core Principles

| Principle | Implementation Strategy |
| :--- | :--- |
| **Zero Latency** | SIMD HTTP Parser · `io_uring`/`kqueue` Batching · `sendfile(2)` · `kTLS` |
| **Zero Allocation** | Per-request Arena · `FixedPoolResource` for event entries · Lock-free RingBuffers |
| **Zero Dependency** | Kernel-native syscalls only · No external library dependencies in public headers. |
| **Shared-Nothing** | Thread-per-core architecture · NUMA-aware pinning · Minimal cross-thread sync. |

---

## 🗺 Documentation Map

- **[Roadmap & Progress](./TODO.md)**: Current status and future milestones.
- **[Strategic Vision](./docs/strategic-evaluation.md)**: Why `qbuem-stack` is a universal platform.
- **[IO Architecture](./docs/io-architecture.md)**: Zero-copy networking and file I/O deep dive.
- **[Pipeline Design](./docs/pipeline-design.md)**: High-performance stream processing engine.
- **[Library Strategy](./docs/library-strategy.md)**: Modular 9-level build system.
- **[SHM Messaging](./docs/shm-messaging.md)**: Sub-microsecond cross-process IPC.
- **[DB Abstraction](./docs/db-abstraction.md)**: Zero-allocation database driver interface.

---

## 🚀 Quick Start

```cpp
#include <qbuem/qbuem_stack.hpp>

int main() {
    qbuem::App app;

    // Direct performance handler
    app.get("/hello", [](const qbuem::Request& req, qbuem::Response& res) {
        res.status(200).body("Zero allocation hello");
    });

    // C++20 Coroutine Async handler
    app.get("/api/v1/user/:id", [](const qbuem::Request& req, qbuem::Response& res) 
        -> qbuem::Task<void> {
        auto id = req.param("id");
        res.status(200).body("{\"id\":\"" + std::string(id) + "\"}");
        co_return;
    });

    return app.listen(8080) ? 0 : 1;
}
```

---

## 🏛 Layered Architecture (The 9 Levels)

1. **Foundation**: `result`, `arena`, `crypto`
2. **Async Core**: `task`, `reactor`, `dispatcher`
3. **IO Primitives**: `net`, `buf`, `file`, `shm`
4. **Transport**: `transport`, `codec`, `server`
5. **Web/HTTP**: `http`, `http-server`
6. **Pipeline**: `context`, `channel`, `pipeline`
7. **Extensions**: `graph`, `resilience`, `db-abstraction`
8. **Protocols**: `ws`, `http2`, `grpc`, `db-pg`
9. **Umbrella**: `qbuem`

---

## 🛠 Feature Support Matrix

| Category | Features |
| :--- | :--- |
| **Network** | TCP/UDP/Unix, `SO_REUSEPORT`, `TCP_FASTOPEN`, `SO_BUSY_POLL` |
| **Performance** | SIMD HTTP/1.1 Parser, SIMD JSON (via qbuem-json), Direct `kqueue`/`iouring` access |
| **Pipeline** | Static/Dynamic Pipelines, Hot-swap, DAG Graphs, Message Bus |
| **Reliability** | Retry, CircuitBreaker, DeadLetterQueue, SLO Tracking |
| **Observability** | W3C TraceContext, Span Exporters (OTLP/Jaeger), SLO Metrics |
| **Advanced** | `AF_XDP` Support, `kTLS` Tx Offload, Zero-copy `sendfile`/`splice` |

---

## 📊 Performance Benchmarks (v1.3.0)

| Component | Metric | Result |
| :--- | :--- | :--- |
| **Event Dispatch** | Latency | ~6μs (macOS/kqueue) ✅ |
| **AsyncChannel** | Throughput | 44M ops/s (MPMC) ✅ |
| **HTTP Parser** | Throughput | 317 MB/s (SIMD) ✅ |
| **Memory Alloc** | `FixedPool` | 4.5 ns / alloc ✅ |
| **Router Lookup** | Latency | 112 ns (RadixTree) ✅ |

---

## 🗓 Roadmap Highlights

- **v1.4.0 (Next)**: Unified Database Abstraction & SHM Messaging Infrastructure.
- **v1.5.0**: Zero-dep Security Core & kTLS optimization.
- **v1.6.0**: Embedded Systems & PCIe userspace integration.
- **v1.7.0**: High-End Connectivity (RDMA/RoCE) & eBPF Observability.

---

*qbuem-stack — Built for mechanical sympathy and extreme performance.*
