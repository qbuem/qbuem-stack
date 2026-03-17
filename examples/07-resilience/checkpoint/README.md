# checkpoint

**Category:** Resilience
**File:** `checkpoint_example.cpp`
**Complexity:** Advanced

## Overview

Demonstrates `CheckpointedPipeline<T>` — a pipeline wrapper that periodically saves processing progress to a `CheckpointStore`. On restart after a crash, the pipeline resumes from the last saved offset instead of reprocessing from the beginning.

## Scenario: Real-Time Log Collection Pipeline

A log pipeline processes events through parse → enrich → persist stages. The pipeline runs continuously; every N items it saves a checkpoint. When the process crashes and restarts, it loads the last checkpoint and skips already-processed events, ensuring **exactly-once** semantics.

## Architecture Diagram

```
  Normal Operation
  ──────────────────────────────────────────────────────────
  LogEvent stream (event_id: 1, 2, 3, ...)
       │
       ▼
  CheckpointedPipeline<LogEvent>
  │  wraps DynamicPipeline
  │  ┌─────────────────────────────────────────────────┐
  │  │  Stage 1: parse   (extract severity from raw)   │
  │  │  Stage 2: enrich  (assign service name)         │
  │  │  Stage 3: persist (increment counter / write)   │
  │  └─────────────────────────────────────────────────┘
  │       │
  │  push_counted(event, context, metadata_json)
  │       │  internally increments items_processed counter
  │       │
  │  every_n=5: save_checkpoint(metadata_json)
  │       │
  │       ▼
  │  InMemoryCheckpointStore
  │  { pipeline_id, offset, metadata_json, saved_at }
  │
  Crash Recovery
  ──────────────────────────────────────────────────────────
  New CheckpointedPipeline instance
       │
       ▼
  resume_from_checkpoint()
  │  loads last checkpoint: {offset=20, metadata=...}
  │  sets items_processed = 20
       │
       ▼
  Continue processing from item 21 onward
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `CheckpointedPipeline<T>(id, store)` | Checkpointed pipeline wrapper |
| `cp.pipeline()` | Access the underlying `DynamicPipeline` |
| `cp.enable_checkpoint(interval, every_n)` | Auto-checkpoint every N items |
| `co_await cp.push_counted(item, ctx, meta)` | Push item and increment counter |
| `cp.items_processed()` | Total items processed since start |
| `co_await cp.save_checkpoint(meta_json)` | Manual checkpoint save |
| `co_await cp.resume_from_checkpoint()` | Load and restore last checkpoint |
| `cp.checkpoint_enabled()` | Check if auto-checkpointing is active |
| `InMemoryCheckpointStore` | In-memory checkpoint storage (for testing) |
| `store.save(id, data)` | Save checkpoint data |
| `store.load(id)` | Load latest checkpoint |
| `store.size()` | Number of stored checkpoints |

## Scenarios

| # | Description | Items | Expected Behavior |
|---|-------------|-------|-------------------|
| 1 | Auto-checkpoint every 5 | 10 | 2 checkpoints saved |
| 2 | Manual checkpoint | 7 | 1 explicit checkpoint |
| 3 | Crash recovery | 20 → resume | New instance resumes at offset=20 |
| 4 | No checkpoint exists | 0 | `resume_from_checkpoint` returns error |

## Input

```
Log messages:
  "INFO: order created"
  "WARN: payment retry"
  "ERROR: stock unavailable"
  ... (10 messages in scenario 1)
```

## Output

```
=== Scenario 1: Batch + auto-checkpoint (every 5) ===
[Result] items_processed=10
[Result] checkpoint_store=2 items
[Checkpoint] offset=10 metadata={"batch":"A","seq":10}

=== Scenario 2: Manual checkpoint ===
  [Checkpoint] manual save complete
[Result] items_processed=7, store=1
[Result] checkpoint_enabled=NO

=== Scenario 3: Resume from checkpoint (crash recovery) ===
[Phase 1] processed=20, checkpoint saved
[Phase 2] pre-resume offset=0
[Phase 2] post-resume offset=20 (processing from item 21)

=== Scenario 4: No checkpoint → error ===
[Result] no checkpoint: no such file or directory (or not_found)
[Result] items_processed=0 (unchanged)

checkpoint_example: ALL OK
```

## How to Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target checkpoint_example
./build/examples/07-resilience/checkpoint/checkpoint_example
```

## Notes

- Replace `InMemoryCheckpointStore` with a persistent store (Redis, PostgreSQL) for production crash recovery.
- `metadata_json` is user-defined; use it to store batch identifiers, watermarks, or consumer group offsets.
- `push_counted` is a wrapper around `DynamicPipeline::push` — it adds the counter increment and optional auto-save.
