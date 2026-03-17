# pipeline_factory

**Category:** Pipeline
**File:** `pipeline_factory_example.cpp`
**Complexity:** Advanced
**Dependencies:** `qbuem-json`

## Overview

Demonstrates `PipelineFactory` — a JSON-driven pipeline builder. Pipeline topology, stage names, concurrency levels, and capacity settings are read from a JSON configuration string or file at runtime. Requires `qbuem-json`.

## Scenario

A DevOps team manages dozens of pipeline variants across environments. Instead of recompiling for each configuration change, they update a JSON config file and the factory reconstructs the pipeline at startup (or on SIGHUP reload).

## Architecture Diagram

```
  JSON Config
  ────────────────────────────────
  {
    "name": "order-pipeline",
    "stages": [
      {"name":"parse",    "concurrency":2, "capacity":64},
      {"name":"validate", "concurrency":1, "capacity":32},
      {"name":"record",   "concurrency":2, "capacity":64}
    ]
  }
       │
       ▼
  PipelineFactory::from_json(config)
       │  maps stage names to registered functions
       │
       ▼
  StaticPipeline (fully configured)
  parse(2) → validate(1) → record(2)
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `PipelineFactory` | JSON-driven pipeline construction |
| `factory.register_stage(name, fn)` | Map a string name to an action function |
| `factory.from_json(json_str)` | Build a pipeline from JSON config |
| `qbuem_json::parse(str)` | JSON parsing (external dependency) |

## Input

JSON configuration string with stage definitions.

## Output

A fully configured and started `StaticPipeline` ready for item processing.

## How to Run

```bash
# Requires qbuem-json (fetched automatically by CMake)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target pipeline_factory_example
./build/examples/05-pipeline/factory/pipeline_factory_example
```

## Notes

- Stage functions must be registered by name before calling `from_json`.
- Config reload without restart: call `from_json` again and `hot_swap` active pipelines.
- Requires `qbuem-json` — the build is skipped if `qbuem-json` is unavailable.
