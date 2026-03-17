# timer_wheel

**Category:** Observability
**File:** `timer_wheel_example.cpp`
**Complexity:** Intermediate

## Overview

Demonstrates `TimerWheel` — an O(1) hierarchical timer implementation. Timers are scheduled with millisecond precision, cancelled atomically, and fired by calling `tick(elapsed_ms)`. Supports zero-delay (immediate) timers and stress-tests 100 concurrent timers.

## Scenario

A reactor event loop needs thousands of concurrent timers (connection timeouts, keep-alive probes, retry delays) with minimal overhead. `TimerWheel` provides O(1) schedule and O(1) cancel — far superior to a sorted heap (O(log N)) for high-timer-count workloads.

## Architecture Diagram

```
  TimerWheel internal structure (simplified)
  ──────────────────────────────────────────────────────────
  Wheel slots: 256 buckets × 8 levels
  Each slot: linked list of timer callbacks

  schedule(delay_ms, callback):
  ├─ compute target_tick = current_tick + delay_ms
  └─ insert into slot[target_tick % 256]  → O(1)

  cancel(timer_id):
  └─ mark timer as cancelled              → O(1)

  tick(elapsed_ms):
  ├─ advance current_tick by elapsed_ms
  ├─ for each newly elapsed slot:
  │   └─ fire all non-cancelled callbacks → O(fired)
  └─ returns number of callbacks fired

  Example timeline:
  ──────────────────────────────────────────────────────────
  t=0ms:   schedule T1(100ms), T2(50ms), T3(200ms), TC(150ms)
           cancel(TC)  → TC will never fire
  t=50ms:  tick(50)  → T2 fires
  t=100ms: tick(50)  → T1 fires
  t=200ms: tick(100) → T3 fires
  t=250ms: tick(50)  → TC slot reached, but cancelled → skipped

  next_expiry_ms():
  └─ returns ms until the nearest scheduled timer
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `TimerWheel` | O(1) hierarchical timer wheel |
| `wheel.schedule(delay_ms, callback)` | Register timer; returns `timer_id` |
| `wheel.cancel(timer_id)` | Cancel timer (O(1)); returns `true` if found |
| `wheel.tick(elapsed_ms)` | Advance wheel and fire due callbacks |
| `wheel.next_expiry_ms()` | Query nearest expiry time |

## Input / Output

| Timer | Delay | Status | Expected fire time |
|-------|-------|--------|-------------------|
| T1 | 100ms | Normal | t=100ms |
| T2 | 50ms | Normal | t=50ms |
| T3 | 200ms | Normal | t=200ms |
| TC | 150ms | **Cancelled** | Never |

```
tick(50ms)   → 1 callback fired  (T2)
tick(50ms)   → 1 callback fired  (T1)
tick(100ms)  → 1 callback fired  (T3)
tick(50ms)   → 0 callbacks fired (TC cancelled)

Fired: ["T2: 50ms", "T1: 100ms", "T3: 200ms"]
```

### Stress Test

| Input | Output |
|-------|--------|
| 100 timers scheduled at delays 1–100ms | All 100 fired after 100 ticks of 1ms each |

## How to Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target timer_wheel_example
./build/examples/08-observability/timer_wheel/timer_wheel_example
```

## Expected Output

```
=== TimerWheel example ===
[timer] cancel(id_cancel)=true
[timer] After 50ms tick: 1 callbacks fired
[timer] T2 fired at 50ms
[timer] After 100ms tick: 1 callbacks fired
[timer] T1 fired at 100ms
[timer] After 200ms tick: 1 callbacks fired
[timer] T3 fired at 200ms
[timer] next_expiry_ms=~300
[timer] Immediate (delay=0) fired: 1
[timer] Stress: 100/100 timers fired
```

## Performance

| Operation | Time Complexity | Typical Cost |
|-----------|----------------|-------------|
| `schedule` | O(1) | < 10 ns |
| `cancel` | O(1) | < 10 ns |
| `tick` | O(fired callbacks) | Proportional to fired count |

## Notes

- `TimerWheel` is **not** thread-safe — call `schedule`, `cancel`, and `tick` from the same reactor thread.
- For cross-thread timer scheduling, post a lambda to the reactor via `dispatcher.post()`.
- `tick(0)` fires any zero-delay timers that were scheduled.
