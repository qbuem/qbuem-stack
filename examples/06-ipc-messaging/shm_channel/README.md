# shm_channel

**Category:** IPC & Messaging
**File:** `shm_channel_example.cpp`
**Complexity:** Intermediate

## Overview

Demonstrates `SHMChannel<T>` (shared-memory ring-buffer channel for inter-process communication) and `SHMBus` (topic-based pub/sub over shared memory), plus `calc_segment_size` for capacity planning.

## Scenario

A producer process writes sensor readings into a shared-memory ring buffer. A consumer process (or thread) receives them with sub-microsecond latency — no TCP, no Unix socket overhead. `SHMBus` extends this to a topic-based publish/subscribe model for multiple message types.

## Architecture Diagram

```
  SHMChannel<T> (direct ring buffer)
  ──────────────────────────────────────────────────────────
  Process A (Producer)                Process B (Consumer)
  ─────────────────────               ─────────────────────
  SHMChannel::create(name, slots)     SHMChannel::open(name)
       │                                   │
       │  try_send(SensorReading)          │  try_recv()
       ▼                                   ▼
  ┌─────────────────────────────────────────────────────┐
  │  /dev/shm/test_sensor_ch                           │
  │  Lock-free ring buffer (8 slots)                   │
  │  SensorReading: {temperature, humidity, sensor_id} │
  └─────────────────────────────────────────────────────┘

  SHMBus (topic-based)
  ──────────────────────────────────────────────────────────
  Publisher                         Subscriber
  ─────────────────────             ─────────────────────
  bus.declare<OrderEvent>(          sub = bus.subscribe<T>(topic)
      "trading.orders",             sub->try_recv()
      LOCAL_ONLY, 64)
  bus.try_publish(topic, event)

  calc_segment_size
  ──────────────────────────────────────────────────────────
  calc_segment_size(item_count, item_size, with_envelope)
  → returns required shm segment bytes (page-aligned)
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `SHMChannel<T>::create(name, slots)` | Create shm ring (producer) |
| `SHMChannel<T>::open(name)` | Open existing shm ring (consumer) |
| `channel->try_send(item)` | Non-blocking enqueue |
| `channel->try_recv()` | Non-blocking dequeue; returns `optional<T*>` |
| `channel->size_approx()` | Approximate number of items in queue |
| `channel->capacity()` | Ring buffer slot count |
| `channel->close()` | Detach from shm segment |
| `SHMChannel<T>::unlink(name)` | Remove shm segment from `/dev/shm` |
| `SHMBus::declare<T>(topic, scope, cap)` | Register a typed topic |
| `SHMBus::try_publish(topic, item)` | Publish to topic |
| `SHMBus::subscribe<T>(topic)` | Subscribe; returns `ISubscription*` |
| `calc_segment_size(n, size, env)` | Compute required shm segment size |

## Input / Output

### SHMChannel Demo

| Operation | Input | Output |
|-----------|-------|--------|
| `try_send` × 3 | `SensorReading{20+i, 55, i}` | `size_approx=3` |
| `try_recv` × 3 | — | `SensorReading` pointers (id=0,1,2) |
| `try_recv` on empty | — | `nullopt` |
| `try_send` after `close()` | any | `false` |

### SHMBus Demo

| Operation | Input | Output |
|-----------|-------|--------|
| `declare` | `"trading.orders"`, 64 slots | `true` |
| `declare` again | same topic | `false` (already declared) |
| `try_publish` × 2 | `OrderEvent{1001, 250.5, 100}` | `true` |
| `try_recv` × 2 | — | `OrderEvent` pointers |

## How to Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target shm_channel_example
./build/examples/06-ipc-messaging/shm_channel/shm_channel_example
```

## Expected Output

```
[SHMChannel] create + try_send/try_recv demo
[SHMChannel] sent 3, size_approx=3
[SHMChannel] recv id=0 temp=20.0
[SHMChannel] recv id=1 temp=21.0
[SHMChannel] recv id=2 temp=22.0
[SHMChannel] OK
[SHMBus] declare + try_publish + subscribe demo
[SHMBus] recv order_id=1001 price=250.5
[SHMBus] OK
[calc_segment_size] s=4096 s1=... s2=... with_env=...
[calc_segment_size] OK
shm_channel_example: ALL OK
```

## Performance

| Metric | Value |
|--------|-------|
| IPC latency (same host) | < 150 ns |
| Throughput | > 10M items/s (same core) |
| Memory overhead per slot | `sizeof(T)` + ring metadata |

## Notes

- `T` must be `trivially_copyable` — no heap pointers, no `std::string`.
- `LOCAL_ONLY` scope means the channel is visible only to processes on the same machine.
- Call `SHMChannel<T>::unlink(name)` from the producer (or a cleanup process) to remove the `/dev/shm/` entry.
- For cross-process pipeline integration, see `ipc_pipeline_example`.
