# io_metrics_dashboard

**Category:** Advanced Applications
**File:** `io_metrics_dashboard.cpp`
**Complexity:** Expert

## Overview

A high-throughput I/O metrics collection and visualization dashboard. Four producer threads each generate 20,000 I/O events (total 80,000), flowing through a `StaticPipeline` (classify → measure → aggregate → snapshot), computing `HistogramMetrics` (p50/p95/p99/p999), and fanning out to metric consumers via `MessageBus`.

## Scenario

An infrastructure monitoring service collects I/O latency events from multiple threads. The pipeline classifies events (READ/WRITE/SYNC), measures latency distributions, aggregates into time windows, and emits periodic snapshots. A metric consumer displays live p-tile statistics.

## Architecture Diagram

```
  4 Producer Threads (each 20,000 events)
  ──────────────────────────────────────────────────────────
  Thread 0: IoEvent{READ,  latency_us=rand}
  Thread 1: IoEvent{WRITE, latency_us=rand}
  Thread 2: IoEvent{READ,  latency_us=rand}
  Thread 3: IoEvent{SYNC,  latency_us=rand}
       │ (80,000 events total)
       ▼
  ┌──────────────────────────────────────────────────────────┐
  │  StaticPipeline                                          │
  │                                                          │
  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌────────┐  │
  │  │ classify │─►│ measure  │─►│aggregate │─►│ snap   │  │
  │  │ READ/    │  │ update   │  │ window   │  │ emit   │  │
  │  │ WRITE/   │  │ histogram│  │ 1000 ev  │  │ period │  │
  │  │ SYNC     │  │          │  │          │  │        │  │
  │  └──────────┘  └──────────┘  └──────────┘  └───┬────┘  │
  └──────────────────────────────────────────────┼───────────┘
                                                 │
                           ┌─────────────────────┤
                           │                     │
                           ▼                     ▼
                    HistogramMetrics      MessageBus
                    p50  p95  p99  p999   "metrics_snapshot"
                    ────────────────────  topic publish
                    5μs  42μs 89μs 180μs
                                          ▼
                                   Metric Consumers
                                   (display / alert)
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `StaticPipeline` (4-stage) | High-throughput classification + aggregation |
| `HistogramMetrics` | HDR-style percentile histogram |
| `histogram.record(us)` | Record a latency sample |
| `histogram.percentile(p)` | Query p-th percentile |
| `MessageBus::publish("metrics_snapshot", snap)` | Fan-out snapshot |
| `Dispatcher(N)` | N-thread reactor |

## Pipeline Stages

| Stage | Purpose | Output |
|-------|---------|--------|
| `classify` | Categorize by I/O type (READ/WRITE/SYNC) | `ClassifiedEvent` |
| `measure` | Update per-type histogram | `MeasuredEvent` |
| `aggregate` | Accumulate until window full (1000 events) | `AggregateWindow` |
| `snap` | Emit snapshot + publish to MessageBus | `MetricsSnapshot` |

## Input

| Source | Count | Rate |
|--------|-------|------|
| 4 producer threads | 20,000 events each = 80,000 total | As fast as possible |

## Output

```
[metrics] Window 1 snapshot:
  READ  p50=5μs p95=42μs p99=89μs p999=180μs (count=400)
  WRITE p50=8μs p95=65μs p99=132μs p999=250μs (count=350)
  SYNC  p50=22μs p95=120μs p99=240μs p999=500μs (count=250)
[MessageBus] metrics_snapshot published → 2 consumers
```

## How to Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target io_metrics_dashboard
./build/examples/11-advanced-apps/io_metrics_dashboard/io_metrics_dashboard
```

## Performance

| Metric | Value |
|--------|-------|
| Input rate | ~2M events/sec |
| Pipeline latency | < 1μs/event |
| Total events | 80,000 |
| Histogram precision | < 1% error at any percentile |

## Notes

- `HistogramMetrics` uses HDR bucketing — accurate across 6 orders of magnitude (1μs to 1s).
- The aggregate window size (1000 events) controls snapshot frequency; smaller windows → more frequent but noisier snapshots.
- This example validates that qbuem-stack pipelines can sustain > 1M events/sec throughput on a single machine.
