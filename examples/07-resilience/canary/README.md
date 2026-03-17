# canary

**Category:** Resilience
**File:** `canary_example.cpp`
**Complexity:** Intermediate

## Overview

Demonstrates `CanaryRouter` for progressive (canary) deployments and `CanaryMetrics` for tracking error rates and latency. Traffic is gradually shifted from a stable version to a canary version, with the ability to rollback instantly.

## Scenario

A new version of an order processing service is deployed. Traffic is initially sent 100% to the stable version. A SRE gradually increases the canary percentage from 0% → 50% → 100%, monitoring error rates and latency at each step. If metrics degrade, `rollback_to_stable()` is called.

## Architecture Diagram

```
  Incoming Requests
       │
       ▼
  ┌────────────────────────────────────────────────────┐
  │  CanaryRouter<Request>                             │
  │                                                    │
  │  canary_percent=0   → all to stable               │
  │  canary_percent=50  → ~50% to canary              │
  │  canary_percent=100 → all to canary               │
  │                                                    │
  │  rollback_to_stable() → canary_percent=0          │
  └─────────────────────┬──────────────────────────────┘
                        │
           ┌────────────┴────────────┐
           ▼  (1-canary_pct)%        ▼  canary_pct%
  ┌─────────────────┐     ┌─────────────────────────┐
  │  Stable Handler │     │  Canary Handler (new v.) │
  │  (current prod) │     │  (under test)            │
  └─────────────────┘     └─────────────────────────┘

  CanaryMetrics
  ──────────────────────────────────────────────────────────
  record_success(latency_us) → tracks success count + avg latency
  record_error()             → tracks error count
  error_rate()               → errors / total
  avg_latency_us()           → running average latency
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `CanaryRouter<T>` | Traffic splitter |
| `router.set_stable(fn)` | Register stable version handler |
| `router.set_canary(fn)` | Register canary version handler |
| `router.set_canary_percent(pct)` | Set canary traffic percentage (0–100) |
| `router.push(item)` | Route an item to stable or canary |
| `router.rollback_to_stable()` | Reset canary percent to 0 |
| `router.canary_percent()` | Read current canary percent |
| `CanaryMetrics::record_success(us)` | Record successful request with latency |
| `CanaryMetrics::record_error()` | Record failed request |
| `CanaryMetrics::error_rate()` | Compute error fraction |
| `CanaryMetrics::avg_latency_us()` | Compute average latency |

## Input / Output

| Scenario | canary_pct | Input (pushes) | Stable | Canary |
|----------|-----------|----------------|--------|--------|
| 1 — 0% canary | 0 | 10 | 10 | 0 |
| 2 — 50% canary | 50 | 1000 | ~500 ±150 | ~500 ±150 |
| 3 — rollback | 0 (after rollback) | 10 | 10 | 0 |
| 4 — 100% canary | 100 | 10 | 0 | 10 |

### CanaryMetrics

| Operation | Input | Output |
|-----------|-------|--------|
| `record_success(100)`, `record_success(200)`, `record_error()` | — | `error_rate≈0.33`, `avg_latency=150μs` |

## How to Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target canary_example
./build/examples/07-resilience/canary/canary_example
```

## Expected Output

```
[CanaryRouter] basic routing demo
[CanaryRouter] 0% canary: stable=10 canary=0
[CanaryRouter] 50% canary: stable=~500 canary=~500
[CanaryRouter] rollback_to_stable: OK
[CanaryMetrics] error_rate=0.333 avg_latency=150 us
[CanaryMetrics] OK
[CanaryRouter] 100% canary: OK
canary_example: ALL OK
```

## Notes

- `CanaryRouter` uses a thread-safe atomic counter for routing decisions — safe to call from multiple threads.
- Combine `CanaryMetrics` with `ErrorBudgetTracker` (see `idempotency_slo_example`) for automated rollback triggers.
