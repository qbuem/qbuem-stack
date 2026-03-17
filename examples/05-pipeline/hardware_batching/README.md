# pipeline_hardware_batching

**Category:** Pipeline
**File:** `pipeline_hardware_batching.cpp`
**Complexity:** Advanced
**Guide Reference:** Pipeline Master Guide §6B (Recipe B)

## Overview

Demonstrates `BatchAction` — a pipeline action that accumulates items into a fixed-size batch and submits them to a hardware accelerator (NPU/GPU analogue) as a single vector call. Covers both the full-batch trigger and the timeout-based partial-batch flush.

## Scenario

An AI inference server receives individual inference requests. The underlying NPU achieves maximum throughput with batch sizes of 8. `BatchAction` accumulates up to 8 requests, then dispatches them together. If fewer than 8 requests arrive within a timeout window, the partial batch is flushed immediately.

## Architecture Diagram

```
  Individual inference requests
  ─────┬────┬────┬────┬────────
       ▼    ▼    ▼    ▼
  ┌─────────────────────────────────┐
  │  BatchAction<InferRequest>      │
  │  batch_size=8, timeout=10ms     │
  │                                 │
  │  accumulate until:              │
  │    (a) batch_size reached  ──►  │ npu_infer(batch[8])
  │    (b) timeout fired       ──►  │ npu_infer(batch[N<8])
  └────────────────┬────────────────┘
                   │  Result<InferResponse> per item
                   ▼
  Pipeline output channel
  (one response per input request)
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `BatchAction<T>(fn, batch_size, timeout)` | Accumulate items and dispatch in batches |
| `batch_size` | Maximum items per hardware dispatch call |
| `timeout_ms` | Flush partial batch after this delay |
| Pipeline integration | `add<U>(batch_action)` in `PipelineBuilder` |

## Input / Output

| Input | Batch Trigger | Output |
|-------|--------------|--------|
| 8 `InferRequest` items | Full batch | 8 `InferResponse` items |
| 3 `InferRequest` items | Timeout flush | 3 `InferResponse` items |

## How to Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target pipeline_hardware_batching
./build/examples/05-pipeline/hardware_batching/pipeline_hardware_batching
```

## Expected Output

```
[batch] dispatching full batch of 8
[batch] dispatching partial batch of 3 (timeout)
[result] 11 responses received
```

## Notes

- `BatchAction` is essential for hardware accelerators (GPU, NPU, FPGA) where per-item dispatch has high overhead.
- The timeout ensures latency bounds are maintained even under low load.
- Combine with `ScatterGatherAction` for fan-out pre-processing before batching.
