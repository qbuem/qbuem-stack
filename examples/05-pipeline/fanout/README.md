# pipeline_fanout

**Category:** Pipeline
**File:** `pipeline_fanout.cpp`
**Complexity:** Intermediate
**Guide Reference:** Pipeline Master Guide §5-1

## Overview

Implements a **Fan-out (Broadcast) / Fan-in (Merge)** topology using `PipelineGraph`. A single input is split into two independent processing branches ("main" and "audit"), and the results from both branches are merged into a single output channel.

## Scenario

A log ingestion service receives raw log lines. Each line must be:
1. Normalized and written to the **main storage** branch.
2. Captured as an **audit record** in a separate compliance branch.

Both processing paths run concurrently; results are collected from a single output channel.

## Architecture Diagram

```
  Input (LogEntry)
       │
       ▼
  ┌───────────┐
  │  ingest   │  source node — passes entry through unchanged
  └─────┬─────┘
        │ fan-out (PipelineGraph edge duplication)
        ├─────────────────────┐
        ▼                     ▼
  ┌───────────┐         ┌───────────┐
  │ normalize │         │   audit   │
  │ branch=   │         │ branch=   │
  │  "main"   │         │ "audit"   │
  └─────┬─────┘         └─────┬─────┘
        │                     │
        └──────────┬──────────┘
                   │ fan-in (both are sinks)
                   ▼
            output channel
         (LogEntry items with
          branch="main" or "audit")

  For N=5 input items:
  Output = 5 "main" + 5 "audit" = 10 total items
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `PipelineGraph<T>` | DAG-structured multi-path pipeline |
| `graph.node(name, fn, concurrency, capacity)` | Register a processing node |
| `graph.edge(from, to)` | Connect nodes (fan-out by adding multiple edges from one node) |
| `graph.source(name)` | Designate the entry-point node |
| `graph.sink(name)` | Designate one or more output nodes (fan-in) |
| `graph.output()` | Get the merged output channel |
| `graph.start(dispatcher)` | Start all nodes |
| `graph.try_push(item)` | Enqueue an item at the source |

## Input

| Field | Type | Example |
|-------|------|---------|
| `raw` | `std::string` | `"log-line-0"` |
| `branch` | `std::string` | `""` (set by action) |
| `stored` | `bool` | `false` (set by action) |

5 `LogEntry` items are pushed (indices 0–4).

## Output

10 `LogEntry` items on the output channel:
- 5 with `branch="main"`, `stored=true`
- 5 with `branch="audit"`, `stored=true`

```
[fan-out] main=5 audit=5
```

## How to Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target pipeline_fanout
./build/examples/05-pipeline/fanout/pipeline_fanout
# Returns 0 on success
```

## Notes

- Fan-out is achieved by calling `graph.edge("ingest", "normalize")` and `graph.edge("ingest", "audit")` — the graph clones the item for each edge.
- Fan-in is achieved by adding both "normalize" and "audit" as sinks; their output is merged into the single `graph.output()` channel.
- The `normalize` node runs with `concurrency=2`; `audit` runs with `concurrency=1`.
