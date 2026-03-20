# qbuem-stack Examples

This directory contains **58 example programs** organized into **11 categories**.
Each example resides in its own subdirectory with a `README.md` covering the
scenario, architecture, key APIs, and expected output.

---

## Quick Navigation

| # | Category | Count | Examples |
|---|----------|:-----:|----------|
| [01](#01-foundation) | Foundation | 3 | hello_world, async_timer, micro_ticker |
| [02](#02-network) | Network | 7 | tcp_echo_server, udp_unix_socket, udp_advanced, websocket, http_fetch, http2_client, fetch_stream |
| [03](#03-memory) | Memory | 4 | arena, zero_copy_arena_channel, numa_hugepages, lockfree_bench |
| [04](#04-codec--security) | Codec & Security | 3 | codec, crypto_url, security_middleware |
| [05](#05-pipeline) | Pipeline | 12 | fanout, dynamic_hotswap, hardware_batching, sensor_fusion, observer_health, factory, subpipeline_migration, stream_ops, windowed_action, dynamic_router, stateful_window, backpressure_monitor |
| [06](#06-ipc--messaging) | IPC & Messaging | 4 | shm_channel, ipc_pipeline, message_bus, priority_spsc_channel |
| [07](#07-resilience) | Resilience | 6 | canary, checkpoint, resilience, saga, scatter_gather, idempotency_slo |
| [08](#08-observability) | Observability | 5 | tracing, lifecycle_tracing, inspector_dashboard, timer_wheel, task_group |
| [09](#09-database) | Database | 2 | db_session, coro_json |
| [10](#10-hardware) | Hardware | 2 | hardware_io, kqueue_sophistication |
| [11](#11-advanced-apps) | Advanced Apps | 10 | autonomous_driving, sensor_fusion, io_metrics_dashboard, trading_platform, game_server, middleware, hft_matching, open_world, spatial_fusion, hardware_chaos |

---

## Building All Examples

```bash
# Configure (fetches qbuem-json automatically for JSON-dependent examples)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build everything
cmake --build build -j$(nproc)

# Build a specific example
cmake --build build --target hello_world
```

> **Note:** Examples requiring `qbuem-json` (`trading_platform`, `game_server`,
> `middleware_example`, `db_session_example`, `pipeline_factory_example`, `coro_json`)
> are automatically fetched. Set `-DQBUEM_JSON_TAG=<tag>` to pin a version.

---

## 01 Foundation

The simplest possible qbuem-stack programs — start here.

| Example | File | Description |
|---------|------|-------------|
| [hello_world](01-foundation/hello_world/) | `hello_world.cpp` | Minimal HTTP server with middleware and routing |
| [async_timer](01-foundation/async_timer/) | `async_timer.cpp` | Sync vs. async coroutine handlers, `co_await sleep()` |
| [micro_ticker](01-foundation/micro_ticker/) | `micro_ticker_example.cpp` | Sub-millisecond precision loop: nanosleep + busy-spin hybrid, MicroTicker driving EpollReactor |

---

## 02 Network

Raw TCP, UDP, Unix-domain socket, WebSocket, and HTTP client protocols.

| Example | File | Description |
|---------|------|-------------|
| [tcp_echo_server](02-network/tcp_echo_server/) | `tcp_echo_server.cpp` | Async TCP echo server with `TcpListener` + `TcpStream` |
| [udp_unix_socket](02-network/udp_unix_socket/) | `udp_unix_socket_example.cpp` | UDP datagrams + Unix-domain socket IPC |
| [udp_advanced](02-network/udp_advanced/) | `udp_advanced_example.cpp` | `UdpMmsgSocket` batch recv/send (64 datagrams/syscall), `RudpSocket` reliable UDP, `MulticastSocket` |
| [websocket](02-network/websocket/) | `websocket_example.cpp` | RFC 6455 handshake, frame encode/decode, NEON/AVX2 masking |
| [http_fetch](02-network/http_fetch/) | `http_fetch_example.cpp` | Monadic fetch: GET/POST, timeout, redirect, `FetchClient` keep-alive pool, kTLS HTTPS |
| [http2_client](02-network/http2_client/) | `http2_client_example.cpp` | HTTP/2 multiplexed client: binary framing, HPACK, SETTINGS, WINDOW_UPDATE, RST_STREAM |
| [fetch_stream](02-network/fetch_stream/) | `fetch_stream_example.cpp` | Zero-copy streaming response via `FetchStream` + fixed 64 KiB chunk pool |

---

## 03 Memory

Zero-allocation memory primitives: arena, pool, zero-copy, NUMA, huge pages,
lock-free data structures.

| Example | File | Description |
|---------|------|-------------|
| [arena](03-memory/arena/) | `arena_example.cpp` | `Arena` (bump-ptr), `FixedPoolResource`, `AsyncLogger` |
| [zero_copy_arena_channel](03-memory/zero_copy_arena_channel/) | `zero_copy_arena_channel_example.cpp` | `sendfile`/`splice` zero-copy + `ArenaChannel<T>` |
| [numa_hugepages](03-memory/numa_hugepages/) | `numa_hugepages_example.cpp` | CPU pinning, huge-page buffer pool, `IoSlice`, NUMA-aware prefetch |
| [lockfree_bench](03-memory/lockfree_bench/) | `lockfree_bench.cpp` | `LockFreeHashMap` vs `mutex+unordered_map`, `IntrusiveList` vs `std::list`, `GenerationPool` vs `shared_ptr` — throughput/latency comparison |

---

## 04 Codec & Security

Protocol codecs and cryptographic utilities — all zero external dependencies.

| Example | File | Description |
|---------|------|-------------|
| [codec](04-codec-security/codec/) | `codec_example.cpp` | `LengthPrefixedCodec` + `LineCodec` (CRLF/LF), partial read handling |
| [crypto_url](04-codec-security/crypto_url/) | `crypto_url_example.cpp` | CSPRNG, RDRAND/RDSEED, CSRF tokens, constant-time compare, URL encode/decode |
| [security_middleware](04-codec-security/security_middleware/) | `security_middleware_example.cpp` | `SecurityMiddleware`, `TokenAuth`, `StaticFiles`, `BodyEncoder` |

---

## 05 Pipeline

The pipeline layer — the core of qbuem-stack's data processing model.
Reference: [Pipeline Master Guide](../docs/pipeline-master-guide.md).

| Example | File | Guide Ref | Description |
|---------|------|-----------|-------------|
| [fanout](05-pipeline/fanout/) | `pipeline_fanout.cpp` | §5-1 | `PipelineGraph` fan-out + fan-in (N→M) |
| [hardware_batching](05-pipeline/hardware_batching/) | `pipeline_hardware_batching.cpp` | §6B | `BatchAction` for NPU/GPU batch dispatch |
| [dynamic_hotswap](05-pipeline/dynamic_hotswap/) | `pipeline_dynamic_hotswap.cpp` | §3-2 | `DynamicPipeline` + live hot-swap + DLQ |
| [sensor_fusion](05-pipeline/sensor_fusion/) | `pipeline_sensor_fusion.cpp` | §6A | N:1 gather with `ServiceRegistry` |
| [observer_health](05-pipeline/observer_health/) | `pipeline_observer_health_example.cpp` | — | `PipelineObserver`, metrics, JSON/DOT/Mermaid export |
| [factory](05-pipeline/factory/) | `pipeline_factory_example.cpp` | — | JSON-driven `PipelineFactory` (requires qbuem-json) |
| [subpipeline_migration](05-pipeline/subpipeline_migration/) | `subpipeline_migration_example.cpp` | — | `SubpipelineAction`, `MigrationAction`, `DlqReprocessor` |
| [stream_ops](05-pipeline/stream_ops/) | `stream_ops_example.cpp` | — | `map`, `filter`, `throttle`, `debounce`, `tumbling_window` operators |
| [windowed_action](05-pipeline/windowed_action/) | `windowed_action_example.cpp` | — | `TumblingWindow`, `SlidingWindow`, `SessionWindow`, `Watermark` |
| [dynamic_router](05-pipeline/dynamic_router/) | `dynamic_router_example.cpp` | — | `DynamicRouter` SIMD predicate: FirstMatch / AllMatch / LoadBalance routing |
| [stateful_window](05-pipeline/stateful_window/) | `stateful_window_example.cpp` | — | `StatefulWindow` thread-local accumulator: TumblingFlush / CountFlush / HybridFlush |
| [backpressure_monitor](05-pipeline/backpressure_monitor/) | `backpressure_monitor_example.cpp` | — | `BackpressureMonitor` — per-stage atomic counters, latency histogram, P50/P99/P99.9 |

---

## 06 IPC & Messaging

Shared-memory channels, message bus pub/sub, and priority channels.

| Example | File | Description |
|---------|------|-------------|
| [shm_channel](06-ipc-messaging/shm_channel/) | `shm_channel_example.cpp` | `SHMChannel<T>` ring buffer + `SHMBus` topic pub/sub |
| [ipc_pipeline](06-ipc-messaging/ipc_pipeline/) | `ipc_pipeline_example.cpp` | **Flagship**: SHMChannel → Pipeline → MessageBus (5 scenarios) |
| [message_bus](06-ipc-messaging/message_bus/) | `message_bus_example.cpp` | `MessageBus` round-robin pub/sub + `try_publish` |
| [priority_spsc_channel](06-ipc-messaging/priority_spsc_channel/) | `priority_spsc_channel_example.cpp` | `PriorityChannel<T>` + `SpscChannel<T>` (lock-free, <10 ns) |

---

## 07 Resilience

Fault tolerance, recovery, and reliability patterns.

| Example | File | Description |
|---------|------|-------------|
| [canary](07-resilience/canary/) | `canary_example.cpp` | `CanaryRouter` progressive deployment + `CanaryMetrics` |
| [checkpoint](07-resilience/checkpoint/) | `checkpoint_example.cpp` | `CheckpointedPipeline` — auto/manual save + crash recovery |
| [resilience](07-resilience/resilience/) | `resilience_example.cpp` | `RetryAction` (exponential + jitter backoff) + `CircuitBreaker` + `DLQ` |
| [saga](07-resilience/saga/) | `saga_example.cpp` | `SagaOrchestrator` — distributed transaction with compensation steps |
| [scatter_gather](07-resilience/scatter_gather/) | `scatter_gather_example.cpp` | `ScatterGatherAction`, `DebounceAction`, `ThrottleAction` |
| [idempotency_slo](07-resilience/idempotency_slo/) | `idempotency_slo_example.cpp` | `IdempotencyFilter` + `ErrorBudgetTracker` + `LatencyHistogram` |

---

## 08 Observability

Distributed tracing, lifecycle monitoring, interactive dashboards, timers, and
structured concurrency.

| Example | File | Description |
|---------|------|-------------|
| [tracing](08-observability/tracing/) | `tracing_example.cpp` | W3C `TraceContext`, `PipelineTracer`, `SpanExporter`, `Sampler` types |
| [lifecycle_tracing](08-observability/lifecycle_tracing/) | `lifecycle_tracing_example.cpp` | `LifecycleTracer<N>` zero-alloc OTLP/SHM tracer, `ShmSpanRing`, `TraceLogger` |
| [inspector_dashboard](08-observability/inspector_dashboard/) | `inspector_dashboard_example.cpp` | `JourneyCollector` Gantt timeline, `inspector_html()` SSE UI, `CoroExplorer`, `AffinityInspector` |
| [timer_wheel](08-observability/timer_wheel/) | `timer_wheel_example.cpp` | `TimerWheel` — O(1) schedule/cancel/tick, 4-level hierarchical wheel |
| [task_group](08-observability/task_group/) | `task_group_example.cpp` | `TaskGroup` — structured concurrency, `join_all<T>()`, `cancel()` |

---

## 09 Database

Database connection pooling and session management.

| Example | File | Description |
|---------|------|-------------|
| [db_session](09-database/db_session/) | `db_session_example.cpp` | `LockFreeConnectionPool` + `InMemorySessionStore` (requires qbuem-json) |
| [coro_json](09-database/coro_json/) | `coro_json.cpp` | Coroutine-based JSON request/response handling (requires qbuem-json) |

---

## 10 Hardware

Low-level hardware integration: PCIe VFIO, RDMA, eBPF, NVMe SPDK, kTLS, kqueue.

| Example | File | Platform | Description |
|---------|------|----------|-------------|
| [hardware_io](10-hardware/hardware_io/) | `hardware_io_example.cpp` | Linux | VFIO PCIe + RDMA + eBPF CO-RE + NVMe SPDK + kTLS kernel offload |
| [kqueue_sophistication](10-hardware/kqueue_sophistication/) | `kqueue_sophistication.cpp` | **macOS only** | Advanced kqueue: `EVFILT_TIMER`, `EVFILT_USER`, batched `kevent64()`, udata dispatch |

---

## 11 Advanced Applications

Full-stack applications combining multiple qbuem-stack layers.

| Example | File | Description |
|---------|------|-------------|
| [autonomous_driving](11-advanced-apps/autonomous_driving/) | `autonomous_driving_fusion.cpp` | 6-sensor EKF+SORT fusion pipeline, AEB hot-swap, MessageBus fan-out |
| [sensor_fusion](11-advanced-apps/sensor_fusion/) | `sensor_fusion_example.cpp` | IMU+GPS+LiDAR static+dynamic pipeline fusion |
| [io_metrics_dashboard](11-advanced-apps/io_metrics_dashboard/) | `io_metrics_dashboard.cpp` | 4 producers × 20k events → p50/p95/p99/p999 metrics + fan-out |
| [trading_platform](11-advanced-apps/trading_platform/) | `trading_platform.cpp` | Full HFT platform: WAS + pipeline + auto-scale + SLO + SSE (requires qbuem-json) |
| [game_server](11-advanced-apps/game_server/) | `game_server.cpp` | Real-time multiplayer game + JWT + rate limit + SSE leaderboard (requires qbuem-json) |
| [middleware](11-advanced-apps/middleware/) | `middleware_example.cpp` | CORS + RateLimit + RequestID + HSTS + BearerAuth + SSE (requires qbuem-json) |
| [hft_matching](11-advanced-apps/hft_matching/) | `hft_matching.cpp` | Lock-free HFT order book: `LockFreeHashMap` symbol registry, `IntrusiveList` price levels, `GenerationPool<Order>`, `MicroTicker`-driven feed at 100 µs |
| [open_world](11-advanced-apps/open_world/) | `open_world_example.cpp` | 5-tick open-world simulation using `TiledBitset<256,256,16>`: aggro radius, AoE count, cross-tile line-of-sight raycast, minimap scan, tile eviction |
| [spatial_fusion](11-advanced-apps/spatial_fusion/) | `spatial_fusion_example.cpp` | Sensor + spatial index fusion: LiDAR occupancy mapped into `GridBitset` for real-time collision queries |
| [hardware_chaos](11-advanced-apps/hardware_chaos/) | `hardware_chaos_example.cpp` | `ChaosHardware` probabilistic fault injection: `ErrorInjection`, `LatencySpike`, `BitFlip`, `PartialWrite`, `DropCompletion` |

---

## Dependency Summary

| Example | Requires `qbuem-json` |
|---------|-----------------------|
| trading_platform | ✓ |
| game_server | ✓ |
| middleware | ✓ |
| db_session | ✓ |
| coro_json | ✓ |
| pipeline_factory | ✓ |
| All others | — |

---

## Learning Paths

**New to qbuem-stack?**
```
01/hello_world → 01/async_timer
    ↓
02/tcp_echo_server → 02/websocket
    ↓
03/arena → 03/zero_copy_arena_channel
    ↓
05/fanout → 05/dynamic_hotswap → 06/ipc_pipeline
    ↓
07/resilience → 07/saga → 07/checkpoint
    ↓
11/trading_platform
```

**AI/ML & Robotics?**
```
05/hardware_batching → 05/sensor_fusion → 11/autonomous_driving → 11/spatial_fusion
```

**FinTech / HFT?**
```
06/shm_channel → 06/ipc_pipeline → 11/hft_matching → 11/trading_platform
```

**Spatial / Game / Simulation?**
```
03/lockfree_bench → 11/open_world → 11/spatial_fusion → 11/game_server
```

**Infrastructure / SRE?**
```
07/resilience → 07/canary → 07/idempotency_slo → 08/tracing → 08/inspector_dashboard
```

**Hardware Engineers?**
```
01/micro_ticker → 10/hardware_io → 11/hft_matching → 11/hardware_chaos
```
