# qbuem-stack: Observability Suite & "qbuem-inspector"

This document specifies the high-performance observability architecture and the visual developer tools suite.

---

## 1. The Observability Philosophy

Traditional observability (heavy-weight logging/tracing) is incompatible with our "Extreme Performance" goals. We use a **Sidecar-Offload** model.

### 1.1 Zero-Allocation Tracing (OTLP)
- **Mechanism**: Spans are written to a thread-local `SHM RingBuffer` using a zero-allocation schema.
- **Offload**: A background "Collector Sidecar" reads from SHM and exports to an OTLP backend (Jaeger, Zipkin, etc.) using `io_uring`.
- **Latency Impact**: < 50ns per span start/end.

### 1.2 Trace-Aware Logging (Unified Observability)
Logs are no longer isolated strings; they are **Contextual Events**.
- **Mechanism**: `AsyncLogger` is extended to support `TraceId` and `SpanId` fields in its ring-buffer entries.
- **Correlation**: Every log line produced via `qbuem_log()` automatically captures the ambient `TraceContext`.
- **Visualization**: The "qbuem-inspector" displays these log events exactly where they occurred in the request timeline.

### 1.3 End-to-End Lifecycle Tracing
A request doesn't always start at the HTTP layer. We provide **Universal Entry/Exit Hooks**.
- **User-Defined Entry**: `Tracer::start_lifecycle(name)` allows users to mark the birth of a request in any layer (e.g., custom TCP protocol, MessageBus trigger, or Hardware IRQ).
- **Ubiquitous Propagation**: The `TraceContext` is automatically carried through:
    - **In-Process**: Coroutine suspension/resume and `Pipeline` stages.
    - **Cross-Process**: Embedded in `SHMChannel` message headers.
    - **Network**: Propagated via W3C TraceContext headers for HTTP/gRPC.
- **Final Exit Visualization**: The "qbuem-inspector" provides a "Full Journey" view, from the first user-marked entry to the final result delivery, highlighting every micro-step and its latency.

---

## 2. Elite Developer Tools

Moving beyond basic tracing, we provide tools for **Hardware-Aware Debugging**.

### 2.1 Core Topology Map (Affinity Inspector)
- **Problem**: Thread migration across NUMA nodes or cores destroys L3 cache locality.
- **Solution**: A real-time map showing each `Reactor` thread, its pinned Core ID, and its current load/IPS (Instructions Per Second).
- **Metric**: Highlights "Inter-NUMA Jitter" in red.

### 2.2 Zero-Copy Buffer Tracker (Heatmap)
- **Problem**: Identifying "Buffer Leaks" in deep, multi-stage pipelines.
- **Solution**: A visual heatmap of the `BufferPool`. 
    - **Green**: Available.
    - **Yellow**: In-flight (active processing).
    - **Red**: Stalled (potential leak/buffer-zombie).

### 2.3 Hardware Chaos Simulator
- **Problem**: Testing error-handling for rare hardware failures (PCIe link down, RDMA timeout).
- **Solution**: A user-space fault-injection layer for VFIO/RDMA calls.
- **Features**: Simulates "Slow Disk", "Corrupted NVMe Completion", and "Network Jitter".

---

## 3. CLI & Web Integrated Viewer

A web-based developer tool (inspired by Flutter DevTools) for real-time stack introspection, complemented by a powerful CLI.

### 3.1 Web-based "qbuem-inspector"
- **Live Dashboard**: Real-time pipeline DAG, metrics, and "Full Journey" tracing.
- **Universal Access**: Can be hosted centrally for production clusters.

### 3.2 Integrated CLI Tool: `qbuem-cli` (Dual Mode)
The CLI tool provides immediate visibility without needing a central dashboard server.

- **TUI Mode (`qbuem-top`)**: 
    - A terminal-native dashboard using ANSI escape codes.
    - Shows real-time core affinity, buffer pool heatmap, and top reactors.
    - Zero-dependency, perfect for SSH/Headless debugging.
- **HTML/Serve Mode (`qbuem-viewer --serve`)**:
    - Starts a local instance of the `qbuem-stack` HTTP server.
    - Hosts the `qbuem-inspector` UI on a local port (e.g., `:9090`).
    - Connects directly to local SHM segments for zero-latency data visualization in the browser.
- **Static Export (`--export`)**: 
    - Generates a self-contained `.html` report with embedded snapshot data for offline analysis or sharing.

### 3.3 Live Pipeline Visualization
- **Graph View**: Real-time DAG of the `PipelineGraph`.
- **Metrics**: Hover over nodes to see RPS, P99 latency, and backpressure (buffer saturation).

### 3.4 Coro-Stack Explorer
- **Insight**: Browses the hierarchy of active `Task<T>` and coroutines.
- **Features**: Visualizes suspension points and helps identify "Stuck" reactors.

### 3.5 Memory Snapshot
- **Arena Usage**: Visual map of `qbuem::Arena` utilization.
- **Leak Detection**: Integration with **eBPF `memleak`** to show un-deallocated blocks in production with < 1% CPU overhead.

---

## 4. Implementation Roadmap

| Feature | Technique | Milestone |
| :--- | :--- | :--- |
| **Trace Core** | Zero-allocation `Span` & SHM Offload | v3.1.0 |
| **Inspector UI** | Web-based Dashboard (React/WASM) | v3.1.0 |
| **Memory Tool** | eBPF memleak bridge | v3.2.0 |
| **Diagnostic** | Coro-Stack state dump | v3.2.0 |

---

## 4. Integration Example

```cpp
// Tracing with UUID-based context propagation
auto ctx = TraceContext::from_headers(req.headers());
auto span = tracer.start_span("process_order", ctx);

// Pipeline automatically attaches span info to the data flow
co_await pipeline.dispatch(order, span);

span.end();
```

---

*qbuem-stack — Making the invisible, visible, without slowing down.*
