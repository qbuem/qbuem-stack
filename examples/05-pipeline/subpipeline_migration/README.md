# subpipeline_migration

**Category:** Pipeline
**File:** `subpipeline_migration_example.cpp`
**Complexity:** Advanced

## Overview

Demonstrates `SubpipelineAction` (composing a sub-pipeline as a single action), `MigrationAction` (type migration between schema versions), and `DlqReprocessor` (replaying dead-letter items through an updated pipeline).

## Scenario

A data pipeline processes `OrderV1` events. After a schema change, `OrderV2` events are introduced. `MigrationAction` transparently upgrades `V1 → V2` in-flight; `SubpipelineAction` encapsulates a validation sub-pipeline as a reusable black box.

## Architecture Diagram

```
  Main Pipeline
  ──────────────────────────────────────────────────────────
  Input: OrderV1 | OrderV2 (mixed)
       │
       ▼
  ┌────────────────────────────────────────────────────┐
  │  MigrationAction<OrderV1, OrderV2>                 │
  │  ├─ OrderV1 → upgrade() → OrderV2                 │
  │  └─ OrderV2 → pass-through                        │
  └──────────────────────────┬─────────────────────────┘
                             │  OrderV2 only
                             ▼
  ┌────────────────────────────────────────────────────┐
  │  SubpipelineAction<OrderV2>                        │
  │  (encapsulates: validate → enrich → score)         │
  └──────────────────────────┬─────────────────────────┘
                             │  ScoredOrder
                             ▼
  ┌────────────────────────────────────────────────────┐
  │  record stage                                      │
  └────────────────────────────────────────────────────┘
                             │
  Failed items → DLQ
       │
       ▼
  DlqReprocessor → replay through updated pipeline
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `SubpipelineAction<T>` | Embed a pipeline as a composable action |
| `MigrationAction<From, To>` | Schema migration in-flight |
| `DlqReprocessor<T>` | Replay DLQ items through a pipeline |

## Input / Output

| Input | Migration | Output |
|-------|----------|--------|
| `OrderV1` | Upgraded to `OrderV2` | `ScoredOrder` |
| `OrderV2` | Pass-through | `ScoredOrder` |
| Failed items (DLQ) | Replayed after fix | Processed successfully |

## How to Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target subpipeline_migration_example
./build/examples/05-pipeline/subpipeline_migration/subpipeline_migration_example
```

## Notes

- `MigrationAction` is zero-overhead when the input type already matches the target type.
- `SubpipelineAction` enables reuse of complex validation logic across multiple parent pipelines without duplication.
