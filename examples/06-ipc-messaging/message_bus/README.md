# message_bus

**Category:** IPC & Messaging
**File:** `message_bus_example.cpp`
**Complexity:** Intermediate

## Overview

Demonstrates `MessageBus` — a lock-free, in-process topic-based pub/sub system. Covers round-robin distribution to multiple subscribers on the same topic, non-blocking `try_publish`, and subscriber lifecycle management.

## Scenario

An order management system publishes order events to a `MessageBus`. Two subscribers (warehouse and notification services) consume the events. Inventory updates are published via the non-blocking `try_publish` path to avoid blocking the hot path.

## Architecture Diagram

```
  Topic: "orders"
  ──────────────────────────────────────────────────────────
  Publisher
  bus.publish("orders", OrderEvent{id, amount, status})
       │
       ▼
  ┌─────────────────────────────────────────────────────┐
  │  MessageBus ("orders" topic)                       │
  │  Round-robin fan-out to active subscribers         │
  └──────────────────────┬──────────────────────────────┘
                         │
            ┌────────────┴────────────┐
            ▼                         ▼
  ┌──────────────────┐     ┌──────────────────┐
  │  Sub1            │     │  Sub2            │
  │  (warehouse      │     │  (notification   │
  │   service)       │     │   service)       │
  └──────────────────┘     └──────────────────┘
  receives ~50% of events    receives ~50% of events
  (round-robin, not broadcast)

  Topic: "inventory"
  ──────────────────────────────────────────────────────────
  bus.try_publish("inventory", InventoryUpdate{sku, delta})
       │  (non-blocking — returns false if queue full)
       ▼
  ┌─────────────────────────────────────────────────────┐
  │  Single subscriber                                  │
  └─────────────────────────────────────────────────────┘
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `MessageBus` | Lock-free in-process pub/sub |
| `bus.start(dispatcher)` | Start the bus on the given dispatcher |
| `bus.subscribe(topic, fn)` | Register a subscriber coroutine |
| `co_await bus.publish(topic, item)` | Async publish (awaitable) |
| `bus.try_publish(topic, item)` | Non-blocking publish; returns `false` if no subscribers or queue full |
| `bus.subscriber_count(topic)` | Query active subscriber count |

## Input

| Call | Payload |
|------|---------|
| `publish("orders", ...)` × 4 | `OrderEvent{1..4, i*99.9, "created"}` |
| `try_publish("inventory", ...)` × 2 | `InventoryUpdate{"SKU-001", +10}`, `{"SKU-002", -5}` |

## Output

```
=== MessageBus Pub/Sub ===
[bus] Sub1: order_id=1 amount=99.9 status=created
[bus] Sub2: order_id=2 amount=199.8
[bus] Sub1: order_id=3 amount=299.7 status=created
[bus] Sub2: order_id=4 amount=399.6
[bus] Received 4 events

[bus] try_publish SKU-001: 1
[bus] try_publish SKU-002: 1
[bus] inventory subscribers: 1
[bus] Inventory: sku=SKU-001 delta=10
[bus] Inventory: sku=SKU-002 delta=-5
[bus] Inventory events received: 2
```

## How to Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target message_bus_example
./build/examples/06-ipc-messaging/message_bus/message_bus_example
```

## Notes

- **Round-robin** (not broadcast): with 2 subscribers, each event goes to exactly one subscriber. For broadcast, use `PipelineGraph` fan-out instead.
- Subscribers are automatically unregistered when their `SubscriptionHandle` is destroyed (RAII).
- `MessageBus` runs **in-process only**; for cross-process messaging use `SHMBus`.
