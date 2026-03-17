# resilience

**Category:** Resilience
**File:** `resilience_example.cpp`
**Complexity:** Advanced

## Overview

The core resilience patterns example. Demonstrates `RetryAction` (exponential / fixed / jitter backoff), `CircuitBreaker` (state machine: Closed → Open → HalfOpen → Closed), `CircuitBreakerAction`, `DeadLetterQueue`, and `DlqAction` — all composed around a simulated payment gateway.

## Scenario

A payment service calls an external PG (Payment Gateway) that occasionally fails. The system must:
1. **Retry** transient failures automatically (up to N times with exponential backoff).
2. **Open the circuit** after repeated failures to prevent cascading overload (fast-fail).
3. **Store failed payments** in a DeadLetterQueue for manual review or later replay.
4. **Recover** after the PG comes back (HalfOpen probe, then Closed).

## Architecture Diagram

```
  Payment Request
       │
       ▼
  ┌────────────────────────────────────────────────────────┐
  │  RetryAction<Payment, PaymentReceipt>                  │
  │  max_attempts=5, base_delay=1ms                        │
  │  strategy=Exponential | Fixed | Jitter                 │
  │                                                        │
  │  Attempt 1 → fail (PG down)   wait 1ms                │
  │  Attempt 2 → fail             wait 2ms (exp)          │
  │  Attempt 3 → SUCCESS ✓                                │
  └────────────────────────────────────────────────────────┘

  CircuitBreaker State Machine
  ──────────────────────────────────────────────────────────
              ┌──────────────────────────────────┐
              │          failure_threshold=3      │
   Closed ────┼──── 3 failures ────────►  Open   │
   (allow)    │                          (block) │
              │         timeout=50ms            │
              │  Open → timeout expired ──►      │
              │              HalfOpen            │
              │  HalfOpen → 2 successes ──► Closed│
              └──────────────────────────────────┘

  Full Composition: DlqAction + CircuitBreaker
  ──────────────────────────────────────────────────────────
  Payment Request
       │
       ▼
  DlqAction (max_attempts=2)
  ├─ attempt 1: CircuitBreaker.allow()? → call_pg()
  │   └─ fail → record_failure() → cb opens
  ├─ attempt 2: CircuitBreaker.allow()? → NO (OPEN) → fast-fail
  └─ push to DeadLetterQueue
       │
       ▼
  DeadLetterQueue<Payment>
  ├─ peek(10) → inspect failed items
  └─ drain()  → replay when PG recovers
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `RetryConfig` | `max_attempts`, `base_delay`, `max_delay`, `strategy` |
| `BackoffStrategy::Exponential` | 1ms, 2ms, 4ms, 8ms, … |
| `BackoffStrategy::Jitter` | Randomized delay to prevent thundering herd |
| `RetryAction<In,Out>(fn, cfg)` | Wraps a coroutine with retry logic |
| `CircuitBreakerConfig` | `failure_threshold`, `success_threshold`, `timeout`, `on_state_change` |
| `CircuitBreaker` | State machine: Closed / Open / HalfOpen |
| `cb.allow_request()` | Returns `false` when Open |
| `cb.record_success()` / `cb.record_failure()` | Update state |
| `DeadLetterQueue<T>(config)` | Stores failed items |
| `dlq.push(item, error)` | Add failed item |
| `dlq.peek(n)` | Inspect without removing |
| `dlq.drain()` | Remove all items for replay |
| `dlq.size()` | Current DLQ depth |
| `DlqAction<In,Out>(fn, dlq, max_attempts)` | Auto-push to DLQ on failure |

## Scenarios

| # | Name | PG failures | Expected result |
|---|------|------------|-----------------|
| 1 | Retry (exponential) | First 2 calls | Succeeds on attempt 3 |
| 2 | CircuitBreaker states | 3 consecutive | Opens, then HalfOpen, then Closed |
| 3 | DlqAction + CB + replay | All calls fail | 5 payments → DLQ; replay succeeds |
| 4 | Jitter backoff | First 4 calls | Succeeds on attempt 5 |

## How to Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target resilience_example
./build/examples/07-resilience/resilience/resilience_example
```

## Expected Output

```
=== [RetryAction] Exponential backoff retry ===
  [PG] call #1 → transient failure (timeout)
  [PG] call #2 → transient failure (timeout)
  [PG] call #3 → approved (payment_id=1001)
[RetryAction] total PG calls: 3 (2 retries + 1 success)

=== [CircuitBreaker] Consecutive failures → OPEN → HalfOpen → Closed ===
  [CB] state transition: Closed → Open
  [CircuitBreaker] OPEN, request blocked: YES
  [CB] HalfOpen: probe allowed
  [CB] state transition: HalfOpen → Closed

=== [DlqAction + CircuitBreaker] Payment pipeline ===
  [DLQ] stored failed payments: 5
[Replay] CB reset, drain 5 → all 5 approved

resilience_example: ALL OK
```

## Notes

- Jitter backoff is recommended when many instances retry simultaneously (prevents thundering herd).
- `CircuitBreaker` state transitions invoke the `on_state_change` callback — ideal for alerting or metric emission.
- `DeadLetterQueue` is bounded by `config.max_size`; when full, `push` fails silently.
