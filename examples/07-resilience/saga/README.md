# saga

**Category:** Resilience
**File:** `saga_example.cpp`
**Complexity:** Advanced

## Overview

Demonstrates `SagaOrchestrator<T>` — a coordinated distributed transaction pattern. Each saga step has a forward `execute` function and a reverse `compensate` function. If any step fails, all previously completed steps are compensated in reverse order.

## Scenario: E-Commerce Order Processing

A customer places an order. The transaction spans three services:
1. **Stock Reservation** — deduct quantity from inventory.
2. **Payment Processing** — charge the credit card.
3. **Shipment Request** — queue the order for dispatch.

If payment fails, the already-deducted stock must be restored. If shipment fails, both the payment must be refunded and the stock must be restored.

## Architecture Diagram

```
  OrderContext flows through saga steps:
  ──────────────────────────────────────────────────────────

  Happy Path (all succeed):
  ┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐
  │ step_reserve    │ ──►│ step_charge     │ ──►│ step_request    │
  │ _stock()        │    │ _payment()      │    │ _shipment()     │
  │ stock -= qty    │    │ PG charge       │    │ queue dispatch  │
  └─────────────────┘    └─────────────────┘    └─────────────────┘
  Result: order complete ✓

  Payment Failure (step 2 fails):
  ┌─────────────────┐    ┌─────────────────┐
  │ reserve_stock   │ ──►│ charge_payment  │ FAIL ✗
  │  ✓              │    │                 │
  └─────────────────┘    └─────────────────┘
             │
             │ compensate in reverse order
             ▼
  ┌─────────────────────────────────────────┐
  │  compensate_reserve_stock()             │
  │  stock += qty  (inventory restored)     │
  └─────────────────────────────────────────┘

  Shipment Failure (step 3 fails):
  All 3 steps run → step 3 fails
  Compensation (reverse order):
  compensate_request_shipment() → cancel dispatch
  compensate_charge_payment()   → refund payment
  compensate_reserve_stock()    → restore inventory
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `SagaStep<In, Out>` | `name`, `execute` (coroutine), `compensate` (coroutine) |
| `SagaOrchestrator<T>` | Orchestrates steps; runs compensations on failure |
| `saga.add_step(step)` | Append a step |
| `co_await saga.run(input, context)` | Execute all steps; returns `Result<T>` |
| `saga.compensation_failures()` | List of steps whose compensation also failed |

## Scenarios

| # | Setup | Failure at | Compensations Run |
|---|-------|-----------|-------------------|
| 1 | Happy path | None | None |
| 2 | `payment_fail_mode=1` | Step 2 (payment) | `compensate_reserve_stock` |
| 3 | `shipping_fail_mode=1` | Step 3 (shipment) | `compensate_request_shipment`, `compensate_charge_payment`, `compensate_reserve_stock` |
| 4 | `stock=1, requested=5` | Step 1 (stock) | None (first step failed before any state changed) |

## Input

```cpp
OrderContext {
    order_id:    1001,
    product_id:  42,
    quantity:    2,
    total_krw:   59800.0,
    card_token:  "tok_visa_4242"
}
```

## Output

```
=== Scenario 1: Normal order ===
  [Stock] reserved — remaining=8
  [Payment] approved — txn=PG-1001 amount=59800
  [Shipment] queued — tracking=SHIP-1001
[Result] success=YES stock=OK payment=OK shipment=OK
[Result] tracking=SHIP-1001, remaining_stock=8

=== Scenario 2: Payment failure → auto stock restore ===
  [Stock] reserved — remaining=7
  [Payment] declined (simulation)
  [Compensation-StockRestore] +3 → remaining=10
[Compensations] stock_restore
[Stock] unchanged=10

=== Scenario 3: Shipment failure → payment refund + stock restore ===
[Compensations] cancel_dispatch payment_refund stock_restore
[Result] stock=10 (restored)
compensation_failures=0

=== Scenario 4: Out of stock (first step fails) ===
[Result] saga failed: no_space_on_device
[Compensations] none (nothing changed)
[Stock] unchanged=1

saga_example: ALL OK
```

## How to Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target saga_example
./build/examples/07-resilience/saga/saga_example
```

## Notes

- `SagaOrchestrator` runs compensations in **strict reverse order** of completed steps.
- If a compensation itself fails, it is recorded in `compensation_failures()` but the remaining compensations continue.
- Use `SagaOrchestrator` for multi-step distributed transactions where partial completion must be undone deterministically.
