# qbuem-stack Pipeline System: The Complete Guide

A complete reference for the `qbuem-stack` Pipeline system.
Covers core architecture, design patterns, IPC integration, and practical recipes.

---

## 1. Overview & Core Philosophy

The Pipeline system follows the "Three Zeros" philosophy: **Zero Latency, Zero Allocation, Zero Dependency**.

### Core Principles

- **Lock-Free by Default**: C++23 coroutines + MPMC/SPSC ring buffer, eliminating mutex.
- **Worker Isolation (Bulkheading)**: Independent worker pool per stage — a single bottleneck does not block the entire pipeline.
- **Natural Backpressure**: When a consumer is saturated, `co_await` suspends the producer at the language level.
- **Mechanical Sympathy**: NUMA-aware scheduling, cache-aligned structs.

---

## 2. System Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  Layer 2: StaticPipeline / DynamicPipeline / PipelineGraph  │
│   PipelineBuilder (with_source · add · with_sink · build)   │
├─────────────────────────────────────────────────────────────┤
│  Layer 1: Action<In, Out>                                   │
│   Worker Pool · ActionEnv · ServiceRegistry                 │
├─────────────────────────────────────────────────────────────┤
│  Layer 0: AsyncChannel<T>                                   │
│   Vyukov MPMC · cross-reactor wakeup via Reactor::post()    │
├─────────────────────────────────────────────────────────────┤
│  Foundation: Dispatcher · Reactor (epoll/kqueue/io_uring)   │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. Static vs. Dynamic Pipelines

### 3-1. StaticPipeline\<In, Out\>

Compile-time fixed type chain. Use on paths where performance is the top priority.

```cpp
auto pipeline = PipelineBuilder<RawOrder, RawOrder>{}
    .add<ParsedOrder>(stage_parse)     // RawOrder  → ParsedOrder
    .add<ParsedOrder>(stage_enrich)    // ParsedOrder → ParsedOrder
    .add<ValidatedOrder>(stage_validate) // ParsedOrder → ValidatedOrder
    .build();   // StaticPipeline<RawOrder, ValidatedOrder>

pipeline.start(dispatcher);
co_await pipeline.push(RawOrder{...});
```

| Property | Value |
| :--- | :--- |
| Type safety | Fully verified at compile time |
| Overhead | Zero (full inlining) |
| Runtime modification | Not supported |

### 3-2. DynamicPipeline\<T\>

Runtime type erasure (`std::any` + virtual dispatch). Use when hot-swap is required.

```cpp
DynamicPipeline<ValidatedOrder> pipeline;
pipeline.add_stage("risk_check", stage_risk_check);
pipeline.add_stage("record",     stage_record);
pipeline.start(dispatcher);

// Runtime stage replacement
pipeline.hot_swap("risk_check", new_risk_check_v2);
```

### Selection Criteria

| Situation | Recommendation |
| :--- | :--- |
| Types determined at compile time | `StaticPipeline` |
| Configuration-file-driven setup | `DynamicPipeline` |
| Runtime logic replacement needed | `DynamicPipeline` + `hot_swap()` |
| Maximum performance required | `StaticPipeline` |

---

## 4. IPC Integration: Three-Layer Messaging Architecture

qbuem-stack fully integrates three messaging layers with the Pipeline.

```
[SHMChannel]  ─── SHMSource ──→  Pipeline  ──→ SHMSink ──→  [SHMChannel]
[MessageBus]  ─── MessageBusSource ──→  Pipeline  ──→ MessageBusSink ──→  [MessageBus]
[SHMBus]      ─── subscribe() + push() ──→  Pipeline
```

### 4-1. PipelineBuilder::with_source()

Connects an external source to the Pipeline head.

```cpp
// SourceT requirements: init() → Result<void>, next() → Task<optional<const T*>>
template <typename SourceT>
PipelineBuilder<OrigIn, CurOut> with_source(SourceT src, size_t cap = 256);
```

- Call `with_source()` before `add()`.
- Internally creates an `AsyncChannel` and registers the source pump coroutine.
- If `init()` fails, the pump does not start (silent failure).

### 4-2. PipelineBuilder::with_sink()

Connects an external sink to the Pipeline tail.

```cpp
// SinkT requirements: init() → Result<void>, sink(const T&) → Task<Result<void>>
template <typename SinkT>
PipelineBuilder<OrigIn, CurOut> with_sink(SinkT snk);
```

- Call `with_sink()` after `add()` and before `build()`.
- The drain coroutine forwards the last stage's output to `snk.sink()`.

### 4-3. MessageBusSource\<T\> / MessageBusSink\<T\>

Connects a MessageBus topic as a Pipeline source or sink.

```cpp
// Subscribe to topic "raw_orders" → Pipeline source
auto pipeline = PipelineBuilder<Order, Order>{}
    .with_source(MessageBusSource<Order>(bus, "raw_orders", 256))
    .add<ProcessedOrder>(stage_process)
    .with_sink(MessageBusSink<ProcessedOrder>(bus, "processed_orders"))
    .build();
```

### 4-4. SHMSource\<T\> / SHMSink\<T\>

Connects a cross-process SHMChannel as a Pipeline source or sink.

```cpp
// Read from a SHMChannel written by another process → Pipeline source
auto pipeline = PipelineBuilder<RawMsg, RawMsg>{}
    .with_source(SHMSource<RawMsg>("ipc.ingress"))
    .add<ProcessedMsg>(stage_process)
    .with_sink(SHMSink<ProcessedMsg>("ipc.egress"))
    .build();
```

### 4-5. Full Integration Example (Trading System)

```
[External Feed Process]
    ↓ SHMChannel<RawOrder>("trading.raw")
[SHMSource → with_source()]
    ↓ stage_parse → stage_enrich → stage_validate
[with_sink() → MessageBusSink → "validated_orders" topic]
    ↓ MessageBus fan-out
[MessageBusSource("validated_orders") → with_source()]
    ↓ DynamicPipeline (stage_risk_check → stage_record)
[output channel consumer]
```

```cpp
// Stage 1: SHM input → MessageBus output
auto stage1 = PipelineBuilder<RawOrder, RawOrder>{}
    .with_source(SHMSource<RawOrder>("trading.raw"))
    .add<ParsedOrder>(stage_parse)
    .add<ParsedOrder>(stage_enrich)
    .add<ValidatedOrder>(stage_validate)
    .with_sink(MessageBusSink<ValidatedOrder>(bus, "validated_orders"))
    .build();

// Stage 2: MessageBus input → DynamicPipeline
DynamicPipeline<ValidatedOrder> stage2;
stage2.add_stage("risk_check", stage_risk_check);
stage2.add_stage("record",     stage_record);

// MessageBus bridge subscription
auto bridge = bus.subscribe("validated_orders",
    [&](MessageBus::Msg msg, Context ctx) -> Task<Result<void>> {
        auto& v = std::any_cast<const ValidatedOrder&>(msg);
        co_return co_await stage2.push(v, ctx);
    });

stage1.start(dispatcher);
stage2.start(dispatcher);
```

---

## 5. Action Implementation

### 5-1. Basic Signature

```cpp
// StaticPipeline stage
Task<Result<Out>> my_stage(In input, ActionEnv env);

// DynamicPipeline stage (homogeneous type)
Task<Result<T>> my_stage(T input, ActionEnv env);
```

### 5-2. Using ActionEnv

```cpp
Task<Result<ParsedOrder>> stage_parse(RawOrder raw, ActionEnv env) {
    // Read context
    auto trace = env.ctx.get<TraceContext>();

    // Access ServiceRegistry
    auto& db = env.services.get<Database>();

    // Check stop token (for long-running operations)
    if (env.stop.stop_requested())
        co_return std::unexpected(std::errc::operation_canceled);

    co_return ParsedOrder{...};
}
```

### 5-3. Action Configuration

```cpp
typename Action<In, Out>::Config cfg {
    .workers    = 4,        // Number of parallel workers (default 1)
    .queue_cap  = 512,      // Input channel capacity
};
builder.add<Out>(my_stage, cfg);
```

---

## 6. Performance & Scheduling

### 6-1. Bulkheading

Assign an independent worker pool to CPU-intensive stages to prevent blocking lightweight stages.

```cpp
auto pipeline = PipelineBuilder<Frame, Frame>{}
    .add<Decoded>(stage_decode, {.workers = 1})   // lightweight
    .add<Result>(stage_npu_infer, {.workers = 8}) // NPU-intensive → independent pool
    .add<Output>(stage_postproc, {.workers = 2})  // lightweight
    .build();
```

### 6-2. Non-blocking Yielding

`co_await` suspends the current coroutine and yields the CPU to other workers.
The reactor continues processing other work even while waiting for I/O.

---

## 7. Pattern Catalog

### 7-1. Fan-out (Broadcast)

```cpp
// MessageBus subscribe provides automatic fan-out
bus.subscribe("events", handler_a);
bus.subscribe("events", handler_b);
bus.subscribe("events", handler_c);
// publish → all three handlers are invoked
co_await bus.publish("events", event);
```

### 7-2. Fan-in (Merge)

```cpp
// Multiple producers push to a shared channel
auto shared_ch = std::make_shared<AsyncChannel<Item>>(256);

dispatcher.spawn(producer_a(shared_ch));  // MPMC — safe
dispatcher.spawn(producer_b(shared_ch));
dispatcher.spawn(consumer(shared_ch));
```

### 7-3. DAG (Directed Acyclic Graph)

```cpp
PipelineGraph graph;
auto a = graph.add_stage("parse",    stage_parse);
auto b = graph.add_stage("enrich",   stage_enrich);
auto c = graph.add_stage("validate", stage_validate);
auto d = graph.add_stage("record",   stage_record);

graph.connect(a, b);
graph.connect(b, c);
graph.connect(c, d);
graph.build_and_start(dispatcher);
```

### 7-4. Feedback Loop

```cpp
// Route to retry queue on failure
Task<Result<Item>> stage_retry_gate(Item item, ActionEnv env) {
    if (item.attempt < 3) {
        // Re-inject into the retry pipeline
        co_await retry_pipeline.push(item);
    }
    co_return item;
}
```

---

## 8. Applied Recipes

### Recipe A: Real-Time Order Processing (SHM → Pipeline → MessageBus)

```cpp
struct RawOrder { uint64_t id; char symbol[16]; double price; int qty; };
// static_assert(std::is_trivially_copyable_v<RawOrder>);

auto pipeline = PipelineBuilder<RawOrder, RawOrder>{}
    .with_source(SHMSource<RawOrder>("exchange.raw_orders"))
    .add<ParsedOrder>(stage_parse)          // char[16] → std::string conversion
    .add<ParsedOrder>(stage_enrich)         // symbol normalization
    .add<ValidatedOrder>(stage_validate)    // notional calculation
    .with_sink(MessageBusSink<ValidatedOrder>(bus, "risk.orders"))
    .build();
```

### Recipe B: Sensor Fusion (N:1 Sync)

```cpp
// Accumulate partial data in ServiceRegistry
Task<Result<FusedData>> stage_fuse(SensorData data, ActionEnv env) {
    auto& store = env.services.get<FusionStore>();
    store.add(data);
    if (!store.is_complete()) co_return std::unexpected(errc::in_progress);
    co_return store.compute_fusion();
}
```

### Recipe C: NPU Batch Processing

```cpp
// Accumulate N frames via WorkerLocal buffer, then run batch inference
Task<Result<InferResult>> stage_npu(Frame frame, ActionEnv env) {
    auto& buf = env.worker_local<std::vector<Frame>>();
    buf.push_back(frame);
    if (buf.size() < 8) co_return std::unexpected(errc::in_progress);
    auto result = co_await npu_infer_batch(buf);
    buf.clear();
    co_return result;
}
```

### Recipe D: Dead Letter Queue

```cpp
Task<Result<Item>> stage_safe(Item item, ActionEnv env) {
    auto res = co_await risky_operation(item);
    if (!res) {
        co_await dlq_pipeline.push(DeadLetter{item, res.error()});
        co_return std::unexpected(res.error());
    }
    co_return *res;
}
```

---

## 9. Periodic Polling Sources

Handles push-style inputs such as sensors and hardware registers.

```cpp
// Direct coroutine loop (without with_source)
dispatcher.spawn([&]() mutable -> Task<void> {
    for (;;) {
        if (auto data = sensor.read(); data.has_value()) {
            co_await pipeline.push(*data);
        }
        co_await qbuem::sleep(1ms);
    }
}());

// Pin to a specific core (NUMA optimization)
dispatcher.spawn_on(0, sensor_loop());
```

---

## 10. Messaging Layer Selection Guide

| Situation | Choice |
| :--- | :--- |
| Between threads in the same process | `AsyncChannel<T>` directly or `MessageBus` |
| Same process, topic-based fan-out | `MessageBus` |
| Between separate processes (IPC) | `SHMChannel<T>` directly or `SHMBus(SYSTEM_WIDE)` |
| Connecting IPC to a Pipeline | `SHMSource<T>` / `SHMSink<T>` + `with_source/sink` |
| Connecting MessageBus to a Pipeline | `MessageBusSource<T>` / `MessageBusSink<T>` |
| Multi-layer chain | Stage 1 pipeline → MessageBusSink → MessageBus → Stage 2 |

---

*qbuem-stack — the optimal infrastructure for high-performance data engineering.*
