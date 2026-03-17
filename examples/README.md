# qbuem-stack Examples

This directory contains **44 example programs** organized into **11 categories**. Each example resides in its own subdirectory with a detailed `README.md` covering the scenario, architecture diagram, key APIs, and input/output specifications.

---

## Quick Navigation

| # | Category | Examples | Difficulty |
|---|----------|---------|-----------|
| [01](#01-foundation) | Foundation | hello_world, async_timer | Beginner |
| [02](#02-network) | Network | tcp_echo_server, udp_unix_socket, websocket | Intermediate |
| [03](#03-memory) | Memory | arena, zero_copy_arena_channel, numa_hugepages | Beginner–Advanced |
| [04](#04-codec--security) | Codec & Security | codec, crypto_url, security_middleware | Intermediate |
| [05](#05-pipeline) | Pipeline | fanout, dynamic_hotswap, hardware_batching, sensor_fusion, observer_health, factory, subpipeline_migration, stream_ops, windowed_action | Intermediate–Advanced |
| [06](#06-ipc--messaging) | IPC & Messaging | shm_channel, ipc_pipeline, message_bus, priority_spsc_channel | Intermediate–Advanced |
| [07](#07-resilience) | Resilience | canary, checkpoint, resilience, saga, scatter_gather, idempotency_slo | Advanced |
| [08](#08-observability) | Observability | tracing, timer_wheel, task_group | Intermediate–Advanced |
| [09](#09-database) | Database | db_session, coro_json | Intermediate |
| [10](#10-hardware) | Hardware | hardware_io, kqueue_sophistication | Expert |
| [11](#11-advanced-apps) | Advanced Apps | autonomous_driving, sensor_fusion, io_metrics_dashboard, trading_platform, game_server, middleware | Advanced–Expert |

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

> **Note:** Examples requiring `qbuem-json` (`trading_platform`, `game_server`, `middleware_example`, `db_session_example`, `pipeline_factory_example`, `coro_json`) are automatically skipped if the network is unavailable. Set `-DQBUEM_JSON_TAG=<tag>` to pin a version.

---

## 01 Foundation

The simplest possible qbuem-stack programs — start here.

| Example | File | Description |
|---------|------|-------------|
| [hello_world](01-foundation/hello_world/README.md) | `hello_world.cpp` | Minimal HTTP server with middleware and routing |
| [async_timer](01-foundation/async_timer/README.md) | `async_timer.cpp` | Sync vs. async coroutine handlers, `co_await sleep()` |

---

## 02 Network

Raw TCP, UDP, Unix-domain socket, and WebSocket protocol handling.

| Example | File | Description |
|---------|------|-------------|
| [tcp_echo_server](02-network/tcp_echo_server/README.md) | `tcp_echo_server.cpp` | Async TCP echo server with `TcpListener` + `TcpStream` |
| [udp_unix_socket](02-network/udp_unix_socket/README.md) | `udp_unix_socket_example.cpp` | UDP datagrams + Unix-domain socket IPC |
| [websocket](02-network/websocket/README.md) | `websocket_example.cpp` | RFC 6455 handshake, frame encode/decode, masking |

---

## 03 Memory

Zero-allocation memory primitives: arena, pool, zero-copy, NUMA, huge pages.

| Example | File | Description |
|---------|------|-------------|
| [arena](03-memory/arena/README.md) | `arena_example.cpp` | `Arena` (bump-ptr), `FixedPoolResource`, `AsyncLogger` |
| [zero_copy_arena_channel](03-memory/zero_copy_arena_channel/README.md) | `zero_copy_arena_channel_example.cpp` | `sendfile`/`splice` zero-copy + `ArenaChannel<T>` |
| [numa_hugepages](03-memory/numa_hugepages/README.md) | `numa_hugepages_example.cpp` | CPU pinning, huge-page buffer pool, IoSlice, prefetch hints |

---

## 04 Codec & Security

Protocol codecs and cryptographic utilities — all zero external dependencies.

| Example | File | Description |
|---------|------|-------------|
| [codec](04-codec-security/codec/README.md) | `codec_example.cpp` | `LengthPrefixedCodec` + `LineCodec` (CRLF/LF), partial read |
| [crypto_url](04-codec-security/crypto_url/README.md) | `crypto_url_example.cpp` | CSPRNG, RDRAND/RDSEED, CSRF tokens, constant-time compare, URL encode/decode |
| [security_middleware](04-codec-security/security_middleware/README.md) | `security_middleware_example.cpp` | SecurityMiddleware, TokenAuth, StaticFiles, BodyEncoder |

---

## 05 Pipeline

The pipeline layer — the core of qbuem-stack's data processing model.

| Example | File | Guide Ref | Description |
|---------|------|-----------|-------------|
| [fanout](05-pipeline/fanout/README.md) | `pipeline_fanout.cpp` | §5-1 | `PipelineGraph` fan-out + fan-in (N→M) |
| [hardware_batching](05-pipeline/hardware_batching/README.md) | `pipeline_hardware_batching.cpp` | §6B | `BatchAction` for NPU/GPU batch dispatch |
| [dynamic_hotswap](05-pipeline/dynamic_hotswap/README.md) | `pipeline_dynamic_hotswap.cpp` | §3-2 + Recipe C | `DynamicPipeline` + live hot-swap + DLQ |
| [sensor_fusion](05-pipeline/sensor_fusion/README.md) | `pipeline_sensor_fusion.cpp` | §6A | N:1 gather with `ServiceRegistry` |
| [observer_health](05-pipeline/observer_health/README.md) | `pipeline_observer_health_example.cpp` | — | `PipelineObserver`, metrics, JSON/DOT/Mermaid export |
| [factory](05-pipeline/factory/README.md) | `pipeline_factory_example.cpp` | — | JSON-driven `PipelineFactory` (requires qbuem-json) |
| [subpipeline_migration](05-pipeline/subpipeline_migration/README.md) | `subpipeline_migration_example.cpp` | — | `SubpipelineAction`, `MigrationAction`, `DlqReprocessor` |
| [stream_ops](05-pipeline/stream_ops/README.md) | `stream_ops_example.cpp` | — | `map`, `filter`, `throttle`, `debounce`, `tumbling_window` |
| [windowed_action](05-pipeline/windowed_action/README.md) | `windowed_action_example.cpp` | — | `TumblingWindow`, `SlidingWindow`, `SessionWindow`, `Watermark` |

---

## 06 IPC & Messaging

Shared-memory channels, message bus pub/sub, and priority channels.

| Example | File | Description |
|---------|------|-------------|
| [shm_channel](06-ipc-messaging/shm_channel/README.md) | `shm_channel_example.cpp` | `SHMChannel<T>` ring buffer + `SHMBus` topic pub/sub |
| [ipc_pipeline](06-ipc-messaging/ipc_pipeline/README.md) | `ipc_pipeline_example.cpp` | **Flagship v2.1.0**: SHMChannel → Pipeline → MessageBus (5 scenarios) |
| [message_bus](06-ipc-messaging/message_bus/README.md) | `message_bus_example.cpp` | `MessageBus` round-robin pub/sub + `try_publish` |
| [priority_spsc_channel](06-ipc-messaging/priority_spsc_channel/README.md) | `priority_spsc_channel_example.cpp` | `PriorityChannel<T>` + `SpscChannel<T>` (lock-free, <10ns) |

---

## 07 Resilience

Fault tolerance, recovery, and reliability patterns.

| Example | File | Description |
|---------|------|-------------|
| [canary](07-resilience/canary/README.md) | `canary_example.cpp` | `CanaryRouter` progressive deployment + `CanaryMetrics` |
| [checkpoint](07-resilience/checkpoint/README.md) | `checkpoint_example.cpp` | `CheckpointedPipeline` — auto/manual save + crash recovery |
| [resilience](07-resilience/resilience/README.md) | `resilience_example.cpp` | `RetryAction` (exp/jitter backoff) + `CircuitBreaker` + `DLQ` |
| [saga](07-resilience/saga/README.md) | `saga_example.cpp` | `SagaOrchestrator` — distributed transaction with compensation |
| [scatter_gather](07-resilience/scatter_gather/README.md) | `scatter_gather_example.cpp` | `ScatterGatherAction`, `DebounceAction`, `ThrottleAction` |
| [idempotency_slo](07-resilience/idempotency_slo/README.md) | `idempotency_slo_example.cpp` | `IdempotencyFilter` + `ErrorBudgetTracker` + `LatencyHistogram` |

---

## 08 Observability

Distributed tracing, timers, and structured concurrency.

| Example | File | Description |
|---------|------|-------------|
| [tracing](08-observability/tracing/README.md) | `tracing_example.cpp` | W3C `TraceContext`, `PipelineTracer`, `SpanExporter`, `Sampler` types |
| [timer_wheel](08-observability/timer_wheel/README.md) | `timer_wheel_example.cpp` | `TimerWheel` — O(1) schedule/cancel/tick |
| [task_group](08-observability/task_group/README.md) | `task_group_example.cpp` | `TaskGroup` — structured concurrency, `join_all<T>()`, `cancel()` |

---

## 09 Database

Database connection pooling and session management.

| Example | File | Description |
|---------|------|-------------|
| [db_session](09-database/db_session/README.md) | `db_session_example.cpp` | `LockFreeConnectionPool` + `InMemorySessionStore` (requires qbuem-json) |
| [coro_json](09-database/coro_json/README.md) | `coro_json.cpp` | Coroutine-based JSON request/response handling (requires qbuem-json) |

---

## 10 Hardware

Low-level hardware integration: PCIe, RDMA, eBPF, NVMe, kTLS, kqueue.

| Example | File | Platform | Description |
|---------|------|----------|-------------|
| [hardware_io](10-hardware/hardware_io/README.md) | `hardware_io_example.cpp` | Linux | VFIO PCIe + RDMA + eBPF + NVMe SPDK + kTLS |
| [kqueue_sophistication](10-hardware/kqueue_sophistication/README.md) | `kqueue_sophistication.cpp` | **macOS only** | Advanced kqueue: EVFILT_TIMER, EVFILT_USER, batched kevent64 |

---

## 11 Advanced Applications

Full-stack applications combining multiple qbuem-stack layers.

| Example | File | Description |
|---------|------|-------------|
| [autonomous_driving](11-advanced-apps/autonomous_driving/README.md) | `autonomous_driving_fusion.cpp` | 6-sensor EKF+SORT fusion pipeline + AEB hot-swap + MessageBus |
| [sensor_fusion](11-advanced-apps/sensor_fusion/README.md) | `sensor_fusion_example.cpp` | IMU+GPS+LiDAR static+dynamic pipeline fusion |
| [io_metrics_dashboard](11-advanced-apps/io_metrics_dashboard/README.md) | `io_metrics_dashboard.cpp` | 4 producers × 20k events → p50/p95/p99/p999 metrics + fan-out |
| [trading_platform](11-advanced-apps/trading_platform/README.md) | `trading_platform.cpp` | Full HFT platform: WAS + pipeline + auto-scale + SLO + SSE (requires qbuem-json) |
| [game_server](11-advanced-apps/game_server/README.md) | `game_server.cpp` | Real-time multiplayer turn-based game + JWT + rate limit + SSE leaderboard (requires qbuem-json) |
| [middleware](11-advanced-apps/middleware/README.md) | `middleware_example.cpp` | CORS + RateLimit + RequestID + HSTS + BearerAuth + SSE (requires qbuem-json) |

---

## Dependency Summary

| Example | Requires qbuem-json |
|---------|:------------------:|
| trading_platform | ✓ |
| game_server | ✓ |
| middleware | ✓ |
| db_session | ✓ |
| coro_json | ✓ |
| pipeline_factory | ✓ |
| All others | ✗ |

---

## Learning Path

**New to qbuem-stack?** Follow this path:

```
01 hello_world → async_timer
       ↓
02 tcp_echo_server → websocket
       ↓
03 arena → zero_copy_arena_channel
       ↓
05 pipeline/fanout → dynamic_hotswap → ipc_pipeline
       ↓
07 resilience → saga → checkpoint
       ↓
11 trading_platform (the full stack)
```

**AI/ML workloads?** Focus on:
```
05/hardware_batching → 05/sensor_fusion → 11/autonomous_driving
```

**FinTech / HFT?**
```
06/shm_channel → 06/ipc_pipeline → 11/trading_platform
```

**Infrastructure / SRE?**
```
07/resilience → 07/canary → 07/idempotency_slo → 08/tracing
```
