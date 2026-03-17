# scatter_gather

**Category:** Resilience
**File:** `scatter_gather_example.cpp`
**Complexity:** Intermediate

## Overview

Demonstrates three event-control actions:
- **`ScatterGatherAction`** — fan-out a single item into sub-items, process in parallel, then gather results.
- **`DebounceAction`** — suppress rapid-fire events; emit only after a quiet gap.
- **`ThrottleAction`** — rate-limit output to a maximum items/second.

## Scenario

1. **ScatterGather**: A sentence is split into words, each word's length is computed in parallel, and the maximum word length is returned.
2. **Debounce**: A search box sends a keystroke event after every character; debounce emits only after the user stops typing for 50ms.
3. **Throttle**: An IoT sensor sends 10 000 events/second; throttle clips the output to 1 000/second.

## Architecture Diagram

```
  ScatterGatherAction
  ──────────────────────────────────────────────────────────
  Input: "hello world foo bar"
       │
       ▼  scatter_fn (split into words)
  ["hello", "world", "foo", "bar"]
       │
       ▼  process_fn (per word, parallel, max_parallel=3)
  [5, 5, 3, 3]  (word lengths)
       │
       ▼  gather_fn (max of lengths)
  Output: 5

  DebounceAction
  ──────────────────────────────────────────────────────────
  Events: e1(t=0ms) e2(t=5ms) e3(t=8ms) e4(t=60ms)
       │  gap=50ms
       ▼
  Output: e3 (last before quiet gap), e4

  ThrottleAction
  ──────────────────────────────────────────────────────────
  Input: 8000 events/s
       │  rate_per_sec=1000, burst=10
       ▼
  Output: ≤1000 events/s (excess dropped)
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `ScatterGatherAction<In, SubIn, SubOut, Out>` | Fan-out + parallel process + gather |
| `action.try_push(item)` | Enqueue input (returns false if channel full) |
| `action.input()->size_approx()` | Queue depth |
| `ScatterGatherConfig.max_parallel` | Limit concurrent process_fn calls |
| `DebounceAction<T>(config)` | Suppress bursts within gap duration |
| `ThrottleAction<T>(config)` | Token-bucket rate limiter |

## Input / Output

### ScatterGatherAction

| Input | scatter → | process → | gather → Output |
|-------|-----------|-----------|----------------|
| `"hello world foo bar"` | 4 words | lengths | `5` (max) |
| `"the quick brown fox"` | 4 words | lengths | `5` (quick) |

### DebounceAction

| Input | Gap | Output |
|-------|-----|--------|
| 5 events in quick succession | 50ms | 5 events queued (emission deferred) |

### ThrottleAction

| Input | Rate | Output |
|-------|------|--------|
| 8 events (within burst=10) | 1000/s, burst=10 | 8 events pass |
| Next 3 events | (burst exhausted) | 0 pass until tokens refill |

## How to Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target scatter_gather_example
./build/examples/07-resilience/scatter_gather/scatter_gather_example
```

## Expected Output

```
[ScatterGatherAction] input queue size=2
[ScatterGatherAction] backpressure: OK
[ScatterGatherAction] OK
[DebounceAction] input=5 events queued
[DebounceAction] OK
[ThrottleAction] input=8 items queued
[ThrottleAction] OK
scatter_gather_example: ALL OK
```

## Notes

- `ScatterGatherAction` is ideal for parallelizing sub-tasks with a maximum concurrency cap to avoid overloading downstream resources.
- `DebounceAction` is useful for search-as-you-type, resize events, and save-on-idle patterns.
- `ThrottleAction` uses a token-bucket algorithm; `burst` controls how many items can pass in a single tick without waiting.
