# pipeline_dynamic_hotswap

**Category:** Pipeline
**File:** `pipeline_dynamic_hotswap.cpp`
**Complexity:** Intermediate
**Guide Reference:** Pipeline Master Guide §3-2 + Recipe C

## Overview

Demonstrates a `DynamicPipeline<T>` with runtime stage hot-swapping and a `DeadLetterQueue`. A "transform" stage is replaced live (without stopping the pipeline) and failed items are captured by the DLQ.

## Scenario

An ETL stream processor starts with a `×2` transform rule. During operation, the business logic changes to `×3+1`. The pipeline is hot-swapped to the new rule without downtime. Negative inputs are rejected by the validate stage; in a full setup they would flow to the DLQ.

## Architecture Diagram

```
  Phase 1 (transform_v1 = ×2)
  ──────────────────────────────────────────────────────────
  Input: [1, 2, 3, 4, 5]
       │
       ▼
  ┌──────────────────────────────────────┐
  │  DynamicPipeline<int>                │
  │  ┌────────────────┐                  │
  │  │  validate      │ x < 0 → error   │
  │  │  (rejects neg) │                  │
  │  └───────┬────────┘                  │
  │          ▼                           │
  │  ┌────────────────┐                  │
  │  │ transform_v1   │ x → x*2         │
  │  └───────┬────────┘                  │
  └──────────┼───────────────────────────┘
             ▼  output: [2, 4, 6, 8, 10]

  ── hot_swap("transform", transform_v2) ──

  Phase 2 (transform_v2 = ×3+1)
  ──────────────────────────────────────────────────────────
  Input: [1, 2, 3, 4, 5]
       │
       ▼
  ┌──────────────────────────────────────┐
  │  DynamicPipeline<int>                │
  │  ┌────────────────┐                  │
  │  │  validate      │                  │
  │  └───────┬────────┘                  │
  │          ▼                           │
  │  ┌────────────────┐                  │
  │  │ transform_v2   │ x → x*3+1       │
  │  └───────┬────────┘                  │
  └──────────┼───────────────────────────┘
             ▼  output: [4, 7, 10, 13, 16]

  Phase 3 (negative inputs → DLQ concept)
  Input: [-1, -2]
       │
       ▼  validate rejects
  DeadLetterQueue<int>
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `DynamicPipeline<T>` | Runtime-configurable homogeneous pipeline |
| `dp.add_stage(name, fn)` | Add a named processing stage |
| `dp.hot_swap(name, new_fn)` | Replace a stage function at runtime (lock-free) |
| `dp.try_push(item)` | Enqueue item non-blocking |
| `dp.output()` | Get output channel |
| `dp.start(dispatcher)` | Start the pipeline |
| `dp.stop()` | Stop gracefully |
| `DeadLetterQueue<T>` | Capture failed items |

## Input / Output

| Phase | Input | Output |
|-------|-------|--------|
| Phase 1 (v1) | `[1,2,3,4,5]` | `[2,4,6,8,10]` |
| Phase 2 (v2, post-swap) | `[1,2,3,4,5]` | `[4,7,10,13,16]` |
| Phase 3 | `[-1,-2]` | Rejected by validate |

## How to Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target pipeline_dynamic_hotswap
./build/examples/05-pipeline/dynamic_hotswap/pipeline_dynamic_hotswap
```

## Expected Output

```
[hot-swap] swapped=true
[phase1] value=2
[phase1] value=4
...
[phase2/hotswap] value=4
[phase2/hotswap] value=7
...
[dynamic-hotswap] phase1_ok=true phase2_ok=true
```

## Notes

- `hot_swap` atomically replaces the stage function pointer; in-flight items already past the stage are unaffected.
- `DynamicPipeline` is best for ETL, config-driven logic, or A/B testing scenarios.
- For static, compile-time pipelines with type-checked chaining, use `StaticPipeline` instead.
