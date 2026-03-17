# qbuem-stack

**Zero Latency · Zero Allocation · Zero Dependency**

> **Current Version: v2.1.0** — All features implemented. Pipeline ↔ MessageBus ↔ SHM fully integrated.
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

## Documentation Map

- **[Roadmap & Progress](./TODO.md)**: Current status and future milestones.
- **[Strategic Vision](./docs/strategic-evaluation.md)**: Why `qbuem-stack` is a universal platform.
- **[Pipeline Master Guide](./docs/pipeline-master-guide.md)**: The complete reference for design, patterns, and IPC integration recipes.
- **[SHM Messaging](./docs/shm-messaging.md)**: Sub-microsecond cross-process IPC with Pipeline bridge adapters.
- **[Library Strategy](./docs/library-strategy.md)**: Modular 9-level build system.
- **[Versatility Guide](./docs/versatility-guide.md)**: Application in Media Streaming, AI/NPU, FinTech, and Edge.
- **[Windows Support](./docs/windows-support.md)**: Native IOCP integration and Win32 alignment.
- **[DB Abstraction](./docs/db-abstraction.md)**: Zero-allocation database driver interface.
- **[Examples Guide](./examples/README.md)**: 44 categorized examples with detailed READMEs.

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

- **v2.1.0 (Current)**: Pipeline ↔ MessageBus ↔ SHM full integration, `SHMChannel::unlink()`, IPC integration tests.
- **v2.2.0 (Next)**: HTTP/3 native (quiche bundled), QUIC transport support.
- **v2.3.0**: AF_XDP production examples, eBPF CO-RE enhancements.

---

*qbuem-stack — Built for mechanical sympathy and extreme performance.*
