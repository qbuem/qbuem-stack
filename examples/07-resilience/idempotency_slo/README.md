# idempotency_slo

**Category:** Resilience
**File:** `idempotency_slo_example.cpp`
**Complexity:** Advanced

## Overview

Demonstrates `IdempotencyFilter` (deduplication of retried requests), `ErrorBudgetTracker` (SLO burn-rate monitoring), and `LatencyHistogram` (p50/p95/p99/p999 percentile tracking) — three production reliability components.

## Scenario

A payment processing endpoint receives duplicate requests due to client retries. `IdempotencyFilter` detects and deduplicates them using an idempotency key. `ErrorBudgetTracker` monitors whether the error rate is burning the monthly SLO budget too fast. `LatencyHistogram` tracks percentile latencies for SLO compliance.

## Architecture Diagram

```
  HTTP POST /payments
  { "idempotency_key": "pay-abc-123", "amount": 50000 }
       │
       ▼
  ┌────────────────────────────────────────────────────────┐
  │  IdempotencyFilter                                     │
  │  ├─ first request:  key not seen → PASS               │
  │  │   └─ process payment, cache result                 │
  │  └─ duplicate:      key seen → REPLAY cached result   │
  │     (idempotent: same response, no double charge)     │
  └──────────────────────────────────────────────────────┘
       │  (new requests only)
       ▼
  ┌────────────────────────────────────────────────────────┐
  │  Payment processing                                    │
  │  ├─ record latency →   LatencyHistogram                │
  │  │   p50, p95, p99, p999                              │
  │  └─ success/error →    ErrorBudgetTracker              │
  │       monthly budget: 0.1% errors                     │
  │       burn_rate > threshold → alert                   │
  └────────────────────────────────────────────────────────┘

  ErrorBudgetTracker
  ──────────────────────────────────────────────────────────
  SLO: 99.9% availability → error budget = 0.1% of requests
  remaining_budget = total_budget - errors_consumed
  burn_rate = current_error_rate / allowed_error_rate
  alert when burn_rate > alert_threshold
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `IdempotencyFilter(ttl)` | Deduplication using idempotency keys |
| `filter.check_or_insert(key)` | Returns cached result if key seen; else inserts |
| `filter.set_result(key, result)` | Cache the response for a key |
| `ErrorBudgetTracker(slo, window)` | Track SLO error budget consumption |
| `tracker.record_request(success)` | Record a request outcome |
| `tracker.remaining_budget()` | Fraction of budget not yet consumed |
| `tracker.burn_rate()` | Current burn rate relative to SLO |
| `LatencyHistogram` | HDR histogram for percentile latencies |
| `histogram.record(value_us)` | Record a latency sample |
| `histogram.percentile(p)` | Query p-th percentile latency |

## Input / Output

### IdempotencyFilter

| Request | Key | Result |
|---------|-----|--------|
| First | `"pay-abc-123"` | Processed, result cached |
| Retry 1 | `"pay-abc-123"` | Cached result returned |
| Retry 2 | `"pay-abc-123"` | Cached result returned |
| New | `"pay-xyz-456"` | Processed normally |

### ErrorBudgetTracker (SLO=99.9%)

| Input | Output |
|-------|--------|
| 1000 requests, 2 errors | `burn_rate=2.0x` (2× faster than allowed) |
| `remaining_budget` | `budget - 2 consumed` |

### LatencyHistogram

| Samples | p50 | p95 | p99 | p999 |
|---------|-----|-----|-----|------|
| 1000 req | ~10ms | ~45ms | ~90ms | ~180ms |

## How to Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target idempotency_slo_example
./build/examples/07-resilience/idempotency_slo/idempotency_slo_example
```

## Notes

- `IdempotencyFilter` TTL controls how long deduplication keys are retained; set based on client retry window (e.g., 24 hours for payment APIs).
- `ErrorBudgetTracker` uses a sliding window; recent errors count more than old errors.
- `LatencyHistogram` uses HDR (High Dynamic Range) buckets — accurate at all scales from microseconds to seconds.
