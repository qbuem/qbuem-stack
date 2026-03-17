# ipc_pipeline

**Category:** IPC & Messaging
**File:** `ipc_pipeline_example.cpp`
**Complexity:** Advanced

## Overview

The flagship IPC integration example. Connects all three messaging layers of qbuem-stack in a single binary: `SHMChannel` (shared-memory IPC), `StaticPipeline` + `DynamicPipeline` (in-process computation), and `MessageBus` (in-process pub/sub). Covers 5 scenarios from simple to fully integrated.

## Scenario: Real-Time Order Processing System

Raw stock orders arrive from an external process via shared memory. They flow through a multi-stage validation pipeline, then fan out to multiple consumers via a message bus, where a second pipeline performs risk checks and records approved orders.

## Architecture Diagram

```
  ┌─────────────────────────────────────────────────────────────────┐
  │  Scenario 4: Full Integration                                   │
  │                                                                 │
  │  External Process                                               │
  │  ┌─────────────┐                                                │
  │  │ SHMChannel  │  (RawOrder — trivially copyable)              │
  │  │  producer   │                                                │
  │  └──────┬──────┘                                                │
  │         │  shm ring buffer                                      │
  │         ▼                                                       │
  │  ┌──────────────────────────────────────────┐                  │
  │  │  StaticPipeline (Stage 1)                │                  │
  │  │  RawOrder → ParsedOrder → ParsedOrder    │                  │
  │  │  → ValidatedOrder                        │                  │
  │  │                                          │                  │
  │  │  parse ── enrich ── validate             │                  │
  │  │                          │               │                  │
  │  │              MessageBusSink("stage1")    │                  │
  │  └──────────────────────────┬───────────────┘                  │
  │                             │                                   │
  │                             ▼  MessageBus fan-out               │
  │  ┌─────────────────────────────────────────────────────────┐   │
  │  │  MessageBus ("stage1_output" topic)                     │   │
  │  │  subscribers: bridge_sub → stage2                       │   │
  │  └──────────────────────────┬──────────────────────────────┘   │
  │                             │                                   │
  │                             ▼                                   │
  │  ┌──────────────────────────────────────────┐                  │
  │  │  DynamicPipeline (Stage 2)               │                  │
  │  │  ValidatedOrder → ValidatedOrder         │                  │
  │  │                                          │                  │
  │  │  risk_check ── record                    │                  │
  │  └──────────────────────────────────────────┘                  │
  └─────────────────────────────────────────────────────────────────┘

  All 5 scenarios:
  ┌────┬──────────────────────────────────────────────────────────┐
  │ 1  │ SHMChannel → SHMSource → StaticPipeline                 │
  │ 2  │ StaticPipeline → MessageBusSink → MessageBus publish    │
  │ 3  │ MessageBusSource → StaticPipeline                       │
  │ 4  │ Full: SHMChannel → StaticPipeline → MessageBus →        │
  │    │       DynamicPipeline                                   │
  │ 5  │ SHMBus (LOCAL_ONLY) → Pipeline bridge                   │
  └────┴──────────────────────────────────────────────────────────┘
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `SHMChannel<T>::create(name, slots)` | Shared-memory ring buffer |
| `SHMSource<T>(name)` | Pipeline source reading from SHMChannel |
| `SHMSink<T>(name)` | Pipeline sink writing to SHMChannel |
| `PipelineBuilder<In,Out>::with_source(src)` | Attach SHMSource / MessageBusSource as pipeline head |
| `PipelineBuilder<In,Out>::with_sink(sink)` | Attach SHMSink / MessageBusSink as pipeline tail |
| `MessageBus::subscribe(topic, fn)` | Subscribe to a topic |
| `MessageBus::publish(topic, item)` | Async publish |
| `MessageBusSink<T>(bus, topic)` | Pipeline tail → MessageBus publish |
| `MessageBusSource<T>(bus, topic)` | MessageBus topic → pipeline source |
| `DynamicPipeline<T>` | Runtime-configurable homogeneous pipeline |

## Domain Types

| Type | Key Fields | Constraint |
|------|-----------|-----------|
| `RawOrder` | `order_id`, `symbol[16]`, `price`, `qty` | Must be `trivially_copyable` (SHM requirement) |
| `ParsedOrder` | `order_id`, `symbol` (string), `valid` | Heap-safe (inside pipeline) |
| `ValidatedOrder` | `base: ParsedOrder`, `notional`, `risk_ok` | — |

## Input

4–5 `RawOrder` items per scenario, including one intentionally invalid order.

## Output

```
=== Scenario 1: SHMChannel → SHMSource → StaticPipeline ===
[Result] parsed=4 validated=3

=== Scenario 2: StaticPipeline → MessageBusSink → MessageBus ===
[Subscriber] order id=2001 notional=2100000
[Result] parsed=3 validated=3 subscriber_received=3

=== Scenario 3: MessageBusSource → StaticPipeline ===
[Record] id=3001 symbol=SAMSUNG qty=10 price=72500 notional=725000
[Result] risk_checked=2 recorded=2 (3003 KAKAO 11M exceeded)

=== Scenario 4: Full Integration ===
[Result] parsed=5 validated=5 risk_passed=4 recorded=4

=== Scenario 5: SHMBus → Pipeline bridge ===
[Result] parsed=3

ipc_pipeline_example: ALL OK
```

## How to Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target ipc_pipeline_example
./build/examples/06-ipc-messaging/ipc_pipeline/ipc_pipeline_example
```

## Notes

- `RawOrder` must not contain `std::string` — use `char[16]` for SHM compatibility.
- `SHMChannel<T>::unlink()` is called at the end of scenario 1 to clean up `/dev/shm` entries.
- The full integration (scenario 4) demonstrates the v2.1.0 flagship architecture.
- Risk threshold: orders with notional > 10,000,000 are rejected by `stage_validate`.
