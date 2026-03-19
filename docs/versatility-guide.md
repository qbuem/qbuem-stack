# Application Versatility: Media, AI, FinTech, Gaming, and Beyond

`qbuem-stack` is a general-purpose high-performance infrastructure engine. Its 9-level modular
architecture and kernel-native principles make it applicable across a wide range of high-demand
domains beyond traditional web servers.

---

## 1. Media Streaming & Real-time Delivery

Ultra-high-definition video/audio streaming demands extreme throughput and minimal jitter.

### 🚀 qbuem-stack Approach

- **Zero-Copy Distribution (Level 3)**: Disk → NIC direct transfer without CPU involvement via `sendfile(2)` + `splice(2)`.
- **UDP / AF_XDP (Level 9)**: For low-latency live streaming (RTP/SRT), bypass the kernel network stack with `AF_XDP` to achieve hundreds of millions of PPS.
- **Pipeline Transcoding (Level 6)**: `StaticPipeline` processes bitstream segmentation or AES encryption in parallel across multiple cores. No locks.
- **Bulkhead Worker Pools**: Dedicated worker pools per encoding stage — lightweight packet handling is never blocked by heavy compute.

```cpp
// Media transcoding pipeline example
auto transcode = PipelineBuilder<RawFrame, RawFrame>{}
    .add<DecodedFrame>(stage_decode,  {.workers = 2})
    .add<ScaledFrame> (stage_scale,   {.workers = 4})  // SIMD scaling
    .add<EncodedFrame>(stage_encode,  {.workers = 8})  // compute-intensive encoding
    .add<Segment>     (stage_segment, {.workers = 2})
    .build();
```

---

## 2. AI & NPU Processing (Hardware Acceleration)

NPUs/GPUs must be fed data at low latency to keep compute units saturated.

### 🧠 qbuem-stack Approach

- **PCIe User-space Integration (Level 7)**: VFIO-based direct PCIe control manages NPU device memory and interrupts from user-space.
- **SHM Messaging (Level 3)**: When an AI model runs in a separate process, `SHMChannel` provides sub-microsecond IPC with zero copies.
- **Pipeline IPC Bridge**: `SHMSource<Frame>` → pre-processing pipeline → `SHMSink<InferResult>` — fully decoupled inference pipeline.
- **Contextual Pipelines (Level 6)**: `Context` attaches metadata such as camera ID and timestamps to frames, then routes them to the NPU stage.

```cpp
// AI inference pipeline: camera SHM → preprocessing → NPU → publish results
struct RawFrame { uint64_t ts; uint32_t cam_id; uint8_t data[1920*1080*3]; };

auto ai_pipeline = PipelineBuilder<RawFrame, RawFrame>{}
    .with_source(SHMSource<RawFrame>("camera.frames"))
    .add<PreprocessedFrame>(stage_preprocess, {.workers = 4})
    .add<InferResult>(stage_npu_infer, {.workers = 8})  // NPU-intensive
    .with_sink(MessageBusSink<InferResult>(bus, "detections"))
    .build();
```

---

## 3. FinTech: High-Frequency Trading & Risk Management

Financial systems requiring nanosecond-level latency and complete audit trails.

### 💹 qbuem-stack Approach

- **SHM IPC Pipeline**: External feed process → `SHMChannel<RawOrder>` → Pipeline → `MessageBus` fan-out — full chain latency < 1μs.
- **StaticPipeline**: Compile-time type chain for zero-overhead order processing (parse → enrich → validate → risk).
- **MessageBus Fan-out**: Delivers a single order event simultaneously to multiple subscribers (risk engine, recording system, monitoring).
- **CircuitBreaker + RetryPolicy**: Automatic circuit breaking and recovery upon external exchange connection failure.
- **JwtAuthAction**: SIMD JWT verification + LRU cache for order requests (< 100ns verification).

```cpp
// Complete trade processing chain
// RawOrder (trivially copyable for SHM)
struct RawOrder { uint64_t id; char symbol[16]; double price; int qty; };

// Stage 1: Market data feed → order validation
auto order_pipe = PipelineBuilder<RawOrder, RawOrder>{}
    .with_source(SHMSource<RawOrder>("exchange.raw_orders"))
    .add<ParsedOrder>  (stage_parse,    {.workers = 2})
    .add<ParsedOrder>  (stage_enrich,   {.workers = 1})  // master data lookup
    .add<ValidatedOrder>(stage_validate, {.workers = 2})
    .with_sink(MessageBusSink<ValidatedOrder>(bus, "risk.orders"))
    .build();

// Stage 2: Risk check + recording
auto risk_pipe = PipelineBuilder<ValidatedOrder, ValidatedOrder>{}
    .with_source(MessageBusSource<ValidatedOrder>(bus, "risk.orders"))
    .add<ValidatedOrder>(stage_risk_check, {.workers = 4})
    .add<ValidatedOrder>(stage_record,     {.workers = 2})
    .build();
```

---

## 4. Industrial Edge & IoT

Edge devices require a small footprint and high reliability.

### 🏭 qbuem-stack Approach

- **Zero Dependency**: Runs on bare Linux/macOS kernels with no external libraries. Ideal for minimal embedded OS images.
- **Resilience Patterns (Level 7)**: `CircuitBreaker` + `RetryPolicy` — keeps the system stable even with intermittent sensor data or network disconnections.
- **NUMA-aware Dispatching**: On advanced edge gateways, pins processing tasks to CPU cores physically close to IO ports.
- **SHMBus LOCAL_ONLY**: Fan-out of sensor data within the same process with zero heap allocation.

```cpp
// Sensor data collection pipeline
SHMBus shm_bus;
shm_bus.declare<SensorReading>("sensors.temp", TopicScope::LOCAL_ONLY, 64);

auto pipeline = PipelineBuilder<SensorReading, SensorReading>{}
    .add<ProcessedReading>(stage_filter)
    .add<Alert>(stage_anomaly_detect)
    .build();
pipeline.start(dispatcher);

// Sensor polling loop
dispatcher.spawn_on(0, [&]() -> Task<void> {  // pinned to core 0
    for (;;) {
        shm_bus.try_publish("sensors.temp", read_sensor());
        co_await qbuem::sleep(100ms);
    }
}());
```

---

## 5. Real-time Game Server (Multiplayer Game Backend)

Multiplayer game backends must simultaneously deliver low latency, high concurrency, and consistent game state.

### 🎮 qbuem-stack Approach

- **StaticPipeline Game Action Chain**: A 3-stage async pipeline (`validate → apply → finalize`) distributes game logic across multiple cores. Each stage has an independent worker pool, so CPU-bound operations (collision detection, AI) never block I/O.
- **MessageBus Broadcast**: Room-level SSE streaming — a single `bus.try_publish("room.{id}", event)` call fans out to all subscribers in that room. Topic isolation ensures no leakage to clients in other rooms.
- **Context Propagation**: A `ResponseChannel` carried inside `Context` through the pipeline allows the final stage to send the HTTP response directly. Cross-reactor safe.
- **DynamicPipeline Operational Flexibility**: Replace the persist stage live via `hot_swap` without downtime, and toggle the replay buffer on/off at runtime via `set_enabled`.
- **RateLimit Anti-cheat**: 60 requests per second per player. Key isolation by IP or token (`max_keys=10,000`).
- **SLO Tracking**: SLO configuration on `stage_finalize` monitors action processing P99 latency.
- **Auto-scaler**: Monitors queue depth every 500ms, adding workers (`scale_out`) under load and reclaiming them (`scale_in`) when idle.

```
[HTTP Client] → Bearer Auth (game-key-{player})
    │
    ├─ POST /api/v1/rooms              Create room
    ├─ POST /api/v1/rooms/:id/join     Join room
    ├─ POST /api/v1/rooms/:id/action   Submit game action
    ├─ GET  /api/v1/rooms/:id/events   SSE real-time events
    └─ GET  /api/v1/leaderboard        Player rankings

[StaticPipeline]
    Action<GameAction, ValidatedAction>  validate  {slo: p99 < 1ms}
    Action<ValidatedAction, StateUpdate> apply     {slo: p99 < 3ms}
    Action<StateUpdate, GameEvent>       finalize

[DynamicPipeline<GameEvent>]
    Stage "persist"   — store event history (hot_swap capable)
    Stage "replay"    — load replay buffer (toggle capable)
    Stage "notify"    — ResponseChannel + MessageBus fan-out

[MessageBus]
    Topic "room.{id}" → per-room SSE streaming
    Topic "leaderboard" → ranking update SSE on game over
```

```cpp
// Game action Nexus Fusion deserialization (zero-tape, 50~230ns)
inline void nexus_pulse(std::string_view key, const char*& p,
                        const char* end, GameAction& g) {
    using namespace qbuem::json::detail;
    switch (fnv1a_hash(key)) {
        case fnv1a_hash_ce("power"):   from_json_direct(p, end, g.power);   break;
        case fnv1a_hash_ce("action"): {
            std::string s; from_json_direct(p, end, s);
            if      (s == "defend")  g.type = ActionType::Defend;
            else if (s == "special") g.type = ActionType::Special;
            else                     g.type = ActionType::Attack;
            break;
        }
        default: skip_direct(p, end); break;
    }
}

// Submit game action → pipeline processing → HTTP response
auto resp_ch = server.submit_action(action, ctx);
auto ev = co_await resp_ch->recv();   // async wait until pipeline completes

// Per-room SSE real-time event streaming
auto stream = bus.subscribe_stream<GameEvent>("room.1", 64);
for (int i = 0; i < 100; ++i) {
    auto ev = co_await stream->recv();
    sse.send(qbuem::write(*ev), ev->game_over ? "game_over" : "action");
    if (ev->game_over) break;
}
```

> **Example file**: [`examples/game_server.cpp`](../examples/game_server.cpp)
>
> ```bash
> # Build
> cmake -DQBUEM_BUILD_EXAMPLES=ON -DQBUEM_JSON_BUILD_BENCHMARKS=OFF ..
> make game_server
>
> # Create room → join → submit action
> curl -H 'Authorization: Bearer game-key-alice' \
>      -X POST http://localhost:8080/api/v1/rooms \
>      -d '{"room_name":"arena-1"}'
>
> curl -H 'Authorization: Bearer game-key-bob' \
>      -X POST http://localhost:8080/api/v1/rooms/1/join
>
> curl -H 'Authorization: Bearer game-key-alice' \
>      -X POST http://localhost:8080/api/v1/rooms/1/action \
>      -d '{"action":"attack","power":8}'
>
> # SSE event streaming
> curl -N http://localhost:8080/api/v1/rooms/1/events
> ```

### Performance Characteristics

| Metric | Value | Notes |
| :--- | :--- | :--- |
| Action processing P99 | < 5ms | validate + apply + finalize combined |
| SSE fan-out latency | < 500μs | MessageBus try_publish |
| Concurrent rooms | Tens of thousands | In-memory GameRegistry, minimal mutex contention |
| Anti-cheat | 60 req/s | RateLimit middleware |

---

## 6. gRPC Microservices

Integrates inter-process gRPC communication with the Pipeline.

### gRPC + Pipeline Integration

```cpp
// gRPC handler routes requests to the Pipeline
auto handler = GrpcHandler<OrderRequest, OrderResponse>{
    [&](Stream<OrderRequest> in, Stream<OrderResponse> out) -> Task<void> {
        async for (auto& req : in) {
            co_await order_pipeline.push(req);
        }
    }
};
```

---

## 6. Summary: The Universal Advantage

| Field | Key qbuem-stack Feature | Key Components |
| :--- | :--- | :--- |
| **Media** | Zero-copy `sendfile`, `AF_XDP`, Stream Pipelines | `zerocopy`, `xdp`, `pipeline` |
| **AI/ML** | SHM IPC, PCIe VFIO, Pipeline Bridge | `shm`, `pcie`, `SHMSource/Sink` |
| **FinTech** | Nano-second latency, Lock-free MPMC, Full IPC chain | `SHMChannel`, `MessageBus`, `pipeline` |
| **Gaming** | StaticPipeline action processing, MessageBus SSE fan-out, Context propagation | `StaticPipeline`, `MessageBus`, `SSE`, `DynamicPipeline` |
| **IoT** | Zero-dependency, Small binary, NUMA-aware | `reactor`, `shm`, `resilience` |
| **Telecom** | AF_XDP 100M+ PPS, QUIC/HTTP3 | `xdp`, `http2` |

---

*qbuem-stack — engineered for platform versatility.*
