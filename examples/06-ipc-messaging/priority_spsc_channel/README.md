# priority_spsc_channel

**Category:** IPC & Messaging
**File:** `priority_spsc_channel_example.cpp`
**Complexity:** Intermediate

## Overview

Demonstrates two specialized channel types:
- **`PriorityChannel<T>`** — a multi-priority queue where high-priority items are always dequeued first.
- **`SpscChannel<T>`** — a single-producer / single-consumer lock-free ring buffer with the lowest possible overhead.

## Scenario

A trading system has two classes of orders: `URGENT` (market orders, must execute immediately) and `NORMAL` (limit orders, can wait). `PriorityChannel` ensures urgent orders are always processed first. For intra-pipeline communication between exactly one producer and one consumer thread, `SpscChannel` provides < 10 ns round-trip.

## Architecture Diagram

```
  PriorityChannel<Order>
  ──────────────────────────────────────────────────────────
  Producer thread
  ┌────────────────────────────────────────────────────┐
  │  push(Order{NORMAL,  "LMT BUY 100 AAPL @ 150"})   │
  │  push(Order{URGENT,  "MKT BUY 50  TSLA"})         │
  │  push(Order{NORMAL,  "LMT SELL 200 GOOG @ 140"})  │
  │  push(Order{URGENT,  "MKT SELL 10 NVDA"})         │
  └────────────────────────┬───────────────────────────┘
                           │
                           ▼
  ┌────────────────────────────────────────────────────┐
  │  PriorityChannel (priority levels: URGENT=0,       │
  │                                    NORMAL=1)        │
  │  dequeue order: URGENT first, then NORMAL          │
  └────────────────────────┬───────────────────────────┘
                           │
                           ▼
  Consumer: receives URGENT orders first
  [URGENT] MKT BUY 50 TSLA
  [URGENT] MKT SELL 10 NVDA
  [NORMAL] LMT BUY 100 AAPL @ 150
  [NORMAL] LMT SELL 200 GOOG @ 140

  SpscChannel<T>
  ──────────────────────────────────────────────────────────
  Single Producer              Single Consumer
  ─────────────────            ─────────────────
  channel.try_send(item)  ──►  channel.try_recv()
  (no lock, cache-friendly)    (no lock, cache-friendly)
  Throughput: >40M ops/s       Latency: <10 ns
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `PriorityChannel<T>(levels, capacity)` | Multi-priority queue |
| `channel.push(item, priority)` | Enqueue with priority level |
| `channel.pop()` | Dequeue highest-priority item |
| `SpscChannel<T>(capacity)` | Lock-free SPSC ring buffer |
| `spsc.try_send(item)` | Non-blocking enqueue (producer) |
| `spsc.try_recv()` | Non-blocking dequeue (consumer) |

## Input / Output

### PriorityChannel

| Pushed (in order) | Received (dequeue order) |
|-------------------|--------------------------|
| NORMAL, URGENT, NORMAL, URGENT | URGENT, URGENT, NORMAL, NORMAL |

### SpscChannel

| Operation | Input | Output |
|-----------|-------|--------|
| `try_send` × N | integers 1..N | `true` (success) |
| `try_recv` × N | — | integers 1..N in order |
| `try_recv` when empty | — | `nullopt` |

## How to Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target priority_spsc_channel_example
./build/examples/06-ipc-messaging/priority_spsc_channel/priority_spsc_channel_example
```

## Performance

| Channel | Throughput | Latency |
|---------|-----------|---------|
| `SpscChannel` | >40M ops/s | <10 ns |
| `AsyncChannel` (MPMC) | ~44M ops/s | ~22 ns |
| `PriorityChannel` | ~5M ops/s | priority sorting overhead |

## Notes

- `SpscChannel` is cache-line aligned and lock-free — ideal for reactor-to-pipeline communication within a single process.
- `PriorityChannel` uses a heap-per-priority-level internally; the number of priority levels is fixed at construction time.
