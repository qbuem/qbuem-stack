# task_group

**Category:** Observability / Concurrency
**File:** `task_group_example.cpp`
**Complexity:** Advanced

## Overview

Demonstrates `TaskGroup` — structured concurrency for C++23 coroutines. Multiple coroutines run concurrently; `join()` waits for all and propagates the first error; `join_all<T>()` collects typed results; `cancel()` propagates stop tokens to all children.

## Scenario: Product Page Data Aggregation (MSA Pattern)

Rendering a product page requires data from four microservices: product info, inventory, pricing, and reviews. All four are fetched concurrently with `TaskGroup`. If any service fails, the error propagates and the page returns an error response.

## Architecture Diagram

```
  Scenario 1: Parallel service fetch (all succeed)
  ──────────────────────────────────────────────────────────
  ┌─────────────────────────────────────────────────────────┐
  │  TaskGroup                                             │
  │  ├─ spawn<ProductInfo>(fetch_product(777))             │
  │  ├─ spawn<InventoryInfo>(fetch_inventory(777))         │
  │  ├─ spawn<PricingInfo>(fetch_pricing(777))             │
  │  └─ spawn<ReviewSummary>(fetch_reviews(777))           │
  │         │                                              │
  │         ▼  all run concurrently                        │
  │  co_await tg.join()  ← waits for all 4                 │
  └─────────────────────────────────────────────────────────┘
  Completes when all 4 coroutines finish

  Scenario 3: One failure propagates
  ──────────────────────────────────────────────────────────
  inventory_service fails (connection_refused)
       │
       ▼
  co_await tg.join() → Result<void>{error=connection_refused}
  → render error page

  Scenario 4: cancel() — immediate abort
  ──────────────────────────────────────────────────────────
  tg.cancel()
  └─ sets stop_source
  spawned tasks check stop_token.stop_requested()
  └─ return operation_canceled immediately

  Scenario 5: join_all<T>() — homogeneous result collection
  ──────────────────────────────────────────────────────────
  5× fetch_inventory(pid) for pids [101..105]
  co_await tg.join_all<InventoryInfo>()
  → Result<vector<InventoryInfo>> (5 items)
  → total_stock = sum of all stocks
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `TaskGroup` | Structured concurrency container |
| `tg.spawn<T>(task)` | Run a `Task<Result<T>>` concurrently |
| `tg.spawn(task)` | Run a `Task<Result<void>>` concurrently |
| `co_await tg.join()` | Wait for all; returns first error |
| `co_await tg.join_all<T>()` | Wait for all; collect `vector<T>` results |
| `tg.cancel()` | Signal all children to stop |
| `tg.stop_token()` | Get the stop token to pass to children |

## Scenarios

| # | Name | Services | Outcome |
|---|------|---------|---------|
| 1 | All succeed | 4 (product, inventory, pricing, reviews) | `join()` OK, 4 service calls |
| 2 | Homogeneous collect | 5 inventory lookups | `join_all<InventoryInfo>()` → 5 results |
| 3 | One failure | inventory fails | `join()` → error propagated |
| 4 | Cancel | 3 long tasks | All cancelled; 0 complete |
| 5 | Fan-out aggregate | 5 warehouses | total stock = sum |
| 6 | Fan-out notify | email+SMS+push+kakao | all 4 channels notified |

## How to Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target task_group_example
./build/examples/08-observability/task_group/task_group_example
```

## Expected Output (excerpt)

```
=== Scenario 1: Parallel fetch (all succeed) ===
  [product]   querying product_id=777
  [inventory] querying product_id=777
  [pricing]   querying product_id=777
  [reviews]   querying product_id=777
  [done] parallel fetch complete (4 service calls)

=== Scenario 3: inventory failure → error propagation ===
  [inventory] fault!
  [result] TaskGroup error: connection_refused
  → render error page

=== Scenario 5: Warehouse aggregation ===
  [warehouse] seoul: 100
  [warehouse] busan: 150  ...
  [aggregate] total stock: 1500 (5 warehouses)

task_group_example: ALL OK
```

## Notes

- `join()` returns the **first** error encountered; other concurrent tasks may still be running at that point (use `cancel()` to abort them).
- `join_all<T>()` requires all spawned tasks to return `Result<T>` with the same `T`; for heterogeneous types, use separate `TaskGroup` instances.
- `stop_token` follows the C++23 `std::stop_token` API — check `stop_requested()` at yield points.
