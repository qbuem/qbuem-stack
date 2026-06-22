# API Consolidation — canonical vs. convenience (v1 → v2)

The pipeline layer grew several overlapping ways to do the same thing. v1 keeps
them all (removing public API would break SemVer), but this guide is the
**authoritative statement of which API is canonical** and which is redundant
"convenience" scheduled for removal in **v2**. Prefer the canonical column.

> Status legend: **Deprecated** = marked `[[deprecated]]` now (compiler warns).
> **Redundant (v2)** = still supported in v1, slated for removal in v2 — migrate
> when convenient. **Keep** = not redundant, stays.

| Area | Redundant / convenience | Canonical replacement | Status | Example |
|---|---|---|---|---|
| Pipeline build from JSON | `PipelineFactory<T>` (`pipeline/pipeline_factory.hpp`) | `StaticPipeline` (typed) or `PipelineGraph` (DAG) built in code | Redundant (v2) | `examples/05-pipeline/fanout`, `examples/11-advanced-apps/*` |
| Version/migration stage | `MigrationAction` / `DlqReprocessor` (`pipeline/migration.hpp`) | A plain `Action<Old,New>` transform stage; `DeadLetterQueue` (`pipeline/resilience/dead_letter.hpp`) for re-drive | Redundant (v2) | `examples/07-resilience/resilience` |
| Windowing (3 impls) | `StatefulWindow` / `make_tumbling_window` (`pipeline/stateful_window.hpp`); `stream_tumbling_window` (`pipeline/stream_ops.hpp`) | `WindowedAction` + `TumblingWindow`/`SlidingWindow`/`SessionWindow` (`pipeline/windowed_action.hpp`) — event-time + watermarks | Redundant (v2) | `examples/05-pipeline/windowed_action` |
| Debounce / throttle (2 impls) | `stream_throttle` / `stream_debounce` operators (`pipeline/stream_ops.hpp`) | `ThrottleAction` / `DebounceAction` (`pipeline/event_actions.hpp`) | Redundant (v2) | `examples/05-pipeline/stream_ops` |
| SLO observer hooks | `SloObserver` / `LoggingSloObserver` (`pipeline/slo.hpp`) | `PipelineObserver` base (`pipeline/observability.hpp`); keep `SloConfig`/`LatencyHistogram`/`ErrorBudgetTracker` | **Deprecated** (zero users) | `examples/05-pipeline/observer_health` |
| Priority channel | `PriorityChannel` (`pipeline/priority_channel.hpp`) | — | **Keep** (real priority semantics `AsyncChannel` lacks) | `examples/06-ipc-messaging/priority_spsc_channel` |

## Notes

- **`SubpipelineAction` name clash (resolved in v2).** There are two unrelated
  `qbuem::SubpipelineAction<In,Out>` classes: the Action form (`operator()`) in
  `pipeline/subpipeline_action.hpp`, and a pipeline-wrapper form (`push`/`start`/
  `drain`) in `pipeline/message_bus.hpp`. No translation unit includes both, so
  there is no active ODR violation; v2 will rename the `message_bus.hpp` one to
  `BusSubpipeline` to remove the hazard. Until then, do not include both headers
  in the same TU.
- **Why deprecate-not-delete in v1.** The `v1` line is being finalized under
  SemVer; removing public symbols is a major-version (v2) change. v1 keeps every
  symbol compiling; this guide + `[[deprecated]]` mark the migration path.
- **Middleware allocation note.** The HTTP middleware layer (`rate_limit`,
  `quota`, `cors`, …) allocates `std::string` per request for keys/headers. This
  is intentional and consistent across the layer: the Zero-Allocation pillar
  targets the Stable core data-plane (Arena/pool/`inplace_function`), not the
  string-oriented HTTP middleware layer.
