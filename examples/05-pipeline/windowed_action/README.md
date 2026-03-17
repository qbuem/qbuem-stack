# windowed_action

**Category:** Pipeline
**File:** `windowed_action_example.cpp`
**Complexity:** Advanced

## Overview

Demonstrates all window types available in qbuem-stack: `TumblingWindow`, `SlidingWindow`, `SessionWindow`, plus `Watermark` and event-time processing for out-of-order event handling.

## Scenario

A real-time analytics engine processes user clickstream events. Different window types are used for different metrics:
- **Tumbling**: 1-minute non-overlapping buckets for minute-level aggregates.
- **Sliding**: 5-minute window advancing every 1 minute for moving averages.
- **Session**: Gaps > 30 minutes close a user session.
- **Watermark**: Handle late-arriving events up to 5 seconds late.

## Architecture Diagram

```
  Event stream (event-time, possibly out of order)
       │
       ▼  assign_timestamps()
  ┌──────────────────────────────────────────────────────┐
  │  Watermark generator                                 │
  │  max_event_time - 5s = current watermark            │
  └──────────────────────────────┬───────────────────────┘
                                 │
        ┌────────────────────────┼─────────────────────────┐
        ▼                        ▼                          ▼
  ┌──────────┐            ┌──────────┐            ┌──────────────┐
  │ Tumbling │            │ Sliding  │            │   Session    │
  │ Window   │            │ Window   │            │   Window     │
  │ 1-min    │            │ 5-min /  │            │ 30-min gap   │
  │ buckets  │            │ 1-min    │            │ timeout      │
  └────┬─────┘            └────┬─────┘            └──────┬───────┘
       │ emit on close         │ emit each slide          │ emit on gap
       ▼                       ▼                          ▼
  TumblingResult          SlidingResult            SessionResult
  {window_start,          {window_end,             {session_id,
   window_end,             moving_avg}              duration,
   count, sum}                                       event_count}
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `TumblingWindow<T>(duration)` | Fixed-size non-overlapping windows |
| `SlidingWindow<T>(size, slide)` | Overlapping windows |
| `SessionWindow<T>(gap)` | Activity-based session detection |
| `Watermark` | Track event-time progress for late event handling |
| `assign_event_time(fn)` | Extract event timestamp from each item |

## Input

Click events: `{user_id, timestamp_ms, page, action}`

## Output

| Window | Output |
|--------|--------|
| `TumblingWindow(60s)` | `{start, end, click_count, unique_users}` per minute |
| `SlidingWindow(5min, 1min)` | Moving average every minute |
| `SessionWindow(30min)` | `{user_id, session_start, session_end, page_count}` |

## How to Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target windowed_action_example
./build/examples/05-pipeline/windowed_action/windowed_action_example
```

## Notes

- `Watermark` is only needed when events can arrive out of order; for in-order streams use processing time directly.
- `SessionWindow` closes when no event arrives within the gap duration; it can produce unbounded state for high-cardinality user sets — use TTL eviction in production.
