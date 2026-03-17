# stream_ops

**Category:** Pipeline
**File:** `stream_ops_example.cpp`
**Complexity:** Intermediate

## Overview

Demonstrates functional stream operators built on top of the pipeline layer: `map`, `filter`, `throttle`, `debounce`, and `tumbling_window`. These compose into a declarative stream processing chain.

## Scenario

A real-time event stream from IoT sensors needs to be filtered for anomalies, transformed, rate-limited, and windowed for aggregation before being sent to a time-series database.

## Architecture Diagram

```
  Raw sensor events (high rate)
       │
       ▼  filter(temperature > 80.0)
  ┌──────────────────────────────────────────┐
  │  Anomaly filter                          │
  │  (drops normal readings)                │
  └────────────────────┬─────────────────────┘
                       │  anomaly events only
                       ▼  map(event → alert)
  ┌──────────────────────────────────────────┐
  │  Transform                              │
  │  (SensorEvent → Alert struct)           │
  └────────────────────┬─────────────────────┘
                       │
                       ▼  throttle(max 100/sec)
  ┌──────────────────────────────────────────┐
  │  Rate limiter                           │
  │  (token bucket)                         │
  └────────────────────┬─────────────────────┘
                       │
                       ▼  debounce(50ms gap)
  ┌──────────────────────────────────────────┐
  │  Debouncer                              │
  │  (suppress bursts)                      │
  └────────────────────┬─────────────────────┘
                       │
                       ▼  tumbling_window(10 items)
  ┌──────────────────────────────────────────┐
  │  Window aggregator                      │
  │  [avg, min, max, count]                 │
  └────────────────────┬─────────────────────┘
                       ▼
  Time-series DB write
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `stream.filter(predicate)` | Drop items that don't match |
| `stream.map(fn)` | Transform each item |
| `stream.throttle(rate)` | Rate-limit output (token bucket) |
| `stream.debounce(gap_ms)` | Suppress bursts within a time gap |
| `stream.tumbling_window(n)` | Collect N items then emit aggregate |

## Input / Output

| Operator | Input | Output |
|----------|-------|--------|
| `filter` | 100 sensor events | ~20 anomalies (temperature > 80) |
| `map` | `SensorEvent` | `Alert{sensor_id, value, severity}` |
| `throttle(100/s)` | 20 alerts/s burst | 20 alerts/s (no change, under limit) |
| `debounce(50ms)` | 5 events in 10ms | 1 event |
| `tumbling_window(10)` | 100 items | 10 aggregates |

## How to Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target stream_ops_example
./build/examples/05-pipeline/stream_ops/stream_ops_example
```

## Notes

- Operators are lazy — they produce pipeline stages and can be chained with `|` or method chaining.
- `tumbling_window` emits when the window is full; use `SlidingWindow` (see `windowed_action`) for overlapping windows.
