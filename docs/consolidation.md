# API Consolidation — removed in 2.0 (migration guide)

The pipeline layer had grown several overlapping ways to do the same thing.
**v2.0.0 removes the redundant surface** (a breaking change — consumers pinned to
1.x are unaffected). This guide maps each removed API to its canonical
replacement.

| Removed in 2.0 | Canonical replacement | Example |
|---|---|---|
| `PipelineFactory<T>` (`pipeline/pipeline_factory.hpp`) | `StaticPipeline` (typed) or `PipelineGraph` (DAG), built in code | `examples/05-pipeline/fanout` |
| `MigrationAction` / `DlqReprocessor` (`pipeline/migration.hpp`) | a plain `Action<Old,New>` transform stage; `DeadLetterQueue` (`pipeline/resilience/dead_letter.hpp`) for re-drive | `examples/07-resilience/resilience` |
| `StatefulWindow` / `make_tumbling_window` / `make_counting_window` (`pipeline/stateful_window.hpp`); `stream_tumbling_window` (`pipeline/stream_ops.hpp`) | `WindowedAction` + `TumblingWindow`/`SlidingWindow`/`SessionWindow` (`pipeline/windowed_action.hpp`) — event-time + watermarks | `examples/05-pipeline/windowed_action` |
| `stream_throttle` / `stream_debounce` (`pipeline/stream_ops.hpp`) | `ThrottleAction` / `DebounceAction` (`pipeline/event_actions.hpp`) | — |
| `SloObserver` / `LoggingSloObserver` (`pipeline/slo.hpp`) | `PipelineObserver` (`pipeline/observability.hpp`); `SloConfig`/`LatencyHistogram`/`ErrorBudgetTracker` are **kept** | `examples/05-pipeline/observer_health` |
| standalone `SubpipelineAction` (`pipeline/subpipeline_action.hpp`) | the `SubpipelineAction` in `pipeline/message_bus.hpp` (kept) — removing the duplicate also resolves a latent ODR hazard | — |

## Kept (not redundant)

- `PriorityChannel` (`pipeline/priority_channel.hpp`) — provides priority-ordered
  delivery that `AsyncChannel` does not; a distinct capability, not redundant.

## Middleware allocation note

The HTTP middleware layer (`rate_limit`, `quota`, `cors`, …) allocates
`std::string` per request for keys/headers. This is intentional and consistent
across the layer: the Zero-Allocation pillar targets the Stable core data-plane
(Arena / pool / `inplace_function`), not the string-oriented HTTP middleware layer.
