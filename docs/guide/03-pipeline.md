# Pipeline System

`include/qbuem/pipeline/` is the largest module in qbuem-stack. It turns the async core (`Dispatcher`, `Task<T>`, `Reactor`) into a complete dataflow toolkit: compile-time typed chains, runtime hot-swappable pipelines, DAG fan-out/fan-in graphs, a family of lock-free channels, resilience wrappers (retry / circuit breaker / DLQ / saga), throughput shapers (batching / windowing / throttle / debounce), Rx-style stream combinators, structured concurrency, and an in-process message bus.

Everything is header-only, zero-dependency (C++23 stdlib + arch intrinsics), and follows the no-exceptions rule: errors are returned as `Result<T>` (`= std::expected<T, std::error_code>`), never thrown. Stage functions are coroutines returning `Task<Result<Out>>`.

Platforms: Linux x86_64, ARM64 boards (Jetson-class), Mac aarch64. The pipeline layer itself contains no platform-specific syscalls — it builds on the portable `AsyncChannel` / `Dispatcher` / `Reactor` primitives. `DynamicRouter` has an AVX2/SSE4.2/NEON fast path with an automatic scalar fallback on any platform.

Umbrella include: `#include <qbuem/pipeline/pipeline.hpp>` pulls the core (action, channel, concepts, context, registry, static_pipeline, stream, task_group). The advanced modules (graph, dynamic, resilience, windowing, etc.) are included individually.

---

## Table of contents

| Group | Types | Header |
|---|---|---|
| Stage contract | `Action<In,Out>`, `ActionEnv`, `ContextualItem<T>`, `WorkerLocal<T>`, `Context`, concepts | `action.hpp`, `action_env.hpp`, `context.hpp`, `concepts.hpp` |
| Pipeline kinds | `StaticPipeline` + `PipelineBuilder`, `DynamicPipeline`, `PipelineGraph` | `static_pipeline.hpp`, `dynamic_pipeline.hpp`, `pipeline_graph.hpp` |
| Channels | `AsyncChannel`, `SpscChannel`, `ArenaChannel`, `PriorityChannel` | `async_channel.hpp`, `spsc_channel.hpp`, `arena_channel.hpp`, `priority_channel.hpp` |
| Resilience | `RetryAction`, `CircuitBreaker(Action)`, `DeadLetterQueue` / `DlqAction`, `SagaOrchestrator`, `IdempotencyFilter` | `retry_policy.hpp`, `circuit_breaker.hpp`, `dead_letter.hpp`, `saga.hpp`, `idempotency.hpp` |
| Throughput / events | `BatchAction`, `DebounceAction`, `ThrottleAction`, `ScatterGatherAction`, `WindowedAction`, `DynamicRouter` | `batch_action.hpp`, `event_actions.hpp`, `windowed_action.hpp`, `dynamic_router.hpp` |
| Streams | `Stream<T>` + `stream_map/filter/chunk/scan/...` | `stream.hpp` |
| Concurrency / wiring | `TaskGroup`, `MessageBus` (+ `MessageBusSource/Sink`), `ServiceRegistry` | `task_group.hpp`, `message_bus.hpp`, `service_registry.hpp` |
| Ops / advanced | `BackpressureMonitor`, `SloConfig`/`ErrorBudgetTracker`, `CanaryRouter`, `CheckpointedPipeline`, health | `backpressure_monitor.hpp`, `slo.hpp`, `canary.hpp`, `checkpoint.hpp`, `health.hpp` |

---

## 1. The stage contract (read this first)

Every pipeline kind reuses the same stage-function contract, validated by concepts in `concepts.hpp`. A stage function is a coroutine. It may take one of three forms (all checked by the `ActionFn<Fn, In, Out>` concept):

```cpp
// Full  — context + cancellation + worker index
Task<Result<Out>> stage(In item, ActionEnv env);
// Simple — cancellation only
Task<Result<Out>> stage(In item, std::stop_token stop);
// Plain  — pure transform
Task<Result<Out>> stage(In item);
```

Internally all three are normalized to the Full signature by `to_full_action_fn<Fn,In,Out>` (`concepts.hpp:127`). There is also a separate batch form validated by `BatchActionFn`: `Task<Result<void>>(std::span<In>, std::span<Out>, ActionEnv)` — but note `BatchAction` (below) actually uses a `vector`-based signature, not the span form.

### `ActionEnv` (`action_env.hpp:42`)

The Full signature receives an `ActionEnv` by value:

```cpp
struct ActionEnv {
  Context          ctx;        // immutable per-item metadata (TraceCtx, RequestId, ...)
  std::stop_token  stop;       // cancellation; set when the stage's stop() is called
  size_t           worker_idx; // 0-based; index into WorkerLocal<T>
  ServiceRegistry *registry;   // never null — falls back to global_registry()
};
```

**Rule (zero-latency, L5/L8):** check `env.stop.stop_requested()` before every suspension point in long stages. `thread_local` is forbidden inside stages — a coroutine can resume on a different thread after `co_await`; use `ctx` slots for per-item state and `WorkerLocal<T>` for per-worker state.

### `Context` (`context.hpp:68`) — immutable per-item metadata

`Context` is an immutable persistent linked list with an inline 4-entry lookup cache. `put<T>()` returns a *new* Context (O(1), shared_ptr prepend); the original is untouched, which makes fan-out safe (`ContextualItem` copies share nodes).

```cpp
Context ctx;
ctx = ctx.put(RequestId{"req-abc"});
ctx = ctx.put(TraceCtx{ /* trace_id, span_id, flags */ });

if (const RequestId* rid = ctx.get_ptr<RequestId>())  // no copy
    use(rid->value);
std::optional<TraceCtx> tc = ctx.get<TraceCtx>();      // copy
```

Built-in slot types (`context.hpp:166+`): `TraceCtx`, `RequestId`, `AuthSubject`, `AuthRoles`, `Deadline`, `ActiveSpan`, `EventTime`, `SagaId`, `IdempotencyKey`. You can `put<>` any of your own types too. Several actions read specific slots: `WindowedAction` reads `EventTime`, `IdempotencyFilter` reads `IdempotencyKey`.

### `ContextualItem<T>` (`action_env.hpp:60`)

The element type that flows *inside* channels — `{ T value; Context ctx; }`. Stage functions receive only `T` (via the channel-pump machinery) and reach the context through `ActionEnv::ctx`.

### `WorkerLocal<T>` (`action_env.hpp:92`) — lock-free per-worker state

`alignas(64)` slots indexed by `env.worker_idx`, so no mutex is needed. Use for per-worker RNGs, scratch buffers, or connection handles. Do **not** use it for per-item state (use `Context`) or pipeline-global state (use `ServiceRegistry`).

```cpp
WorkerLocal<std::mt19937> rngs(dispatcher.thread_count());
// inside a stage:
auto& rng = rngs[env.worker_idx];
```

### Error model

Return `std::unexpected(std::make_error_code(std::errc::...))` (alias `qbuem::unexpected<E>`) on failure, a value on success. Handle errors at the call site:

```cpp
auto r = co_await pipeline.push(item);
if (!r) {                       // r.has_value() == false
    log_error(r.error());       // std::error_code
    co_return std::unexpected(r.error());
}
```

When a `StaticPipeline`/`DynamicPipeline`/`PipelineGraph` *worker* gets an errored result from a stage, the item is silently dropped (the inline comment in `action.hpp:267` notes DLQ integration is opt-in via `DlqAction`, not automatic). If you need failed items captured, wrap the stage in `DlqAction` (§5.3).

---

## 2. `Action<In, Out>` — the worker-pool stage

`Action<In,Out>` (`action.hpp:58`) is the building block under `StaticPipeline`. It owns an input `AsyncChannel<ContextualItem<In>>`, spawns a pool of worker coroutines on a `Dispatcher`, applies the stage function, and forwards results to an output channel.

**When to use directly:** rarely — `PipelineBuilder` constructs `Action`s for you. Use it standalone when you want a single autoscaling stage with manual wiring, or via `make_dynamic_action` to embed it in a `DynamicPipeline`.

**`Action::Config`** (`action.hpp:67`):

| Field | Default | Meaning |
|---|---|---|
| `min_workers` | `1` | workers spawned at `start()` |
| `max_workers` | `4` | ceiling for autoscale |
| `channel_cap` | `256` | input channel capacity (rounded up to pow2) |
| `auto_scale` | `true` | load-based scaling flag |
| `keyed_ordering` | `false` | preserve ordering per key |
| `registry` | `nullptr` | overrides `global_registry()` |
| `slo` | `std::nullopt` | optional `SloConfig` (§7.2) |

Lifecycle: `start(dispatcher, out_channel)` → `push()`/`try_push()` → `drain()` (closes input, waits for all workers, then closes output) or `stop()` (request_stop + close input immediately). `scale_to(n, dispatcher)`, `scale_out(dispatcher)`, `scale_in()` adjust the pool live.

```cpp
Action<int, int> doubler(
    [](int x, ActionEnv) -> Task<Result<int>> { co_return x * 2; },
    {.min_workers = 2, .max_workers = 8, .channel_cap = 1024});

auto out = std::make_shared<AsyncChannel<ContextualItem<int>>>(1024);
doubler.start(dispatcher, out);
co_await doubler.push(21);
auto item = co_await out->recv();   // std::optional<ContextualItem<int>>
```

**Gotchas:** `Action` is move-only (non-copyable). The output channel is closed automatically only after `drain()`; `stop()` closes the *input* but relies on the last worker to close the output. Errored results are dropped (no DLQ unless wrapped).

---

## 3. Choosing a pipeline kind

| Kind | Topology | Type model | Stages change at runtime? | Pick when |
|---|---|---|---|---|
| **`StaticPipeline`** | linear chain | compile-time typed (`In`→…→`Out`) | no | the chain and its types are known at build time; you want max type-safety and zero runtime type-erasure |
| **`DynamicPipeline<T>`** | linear chain | single homogeneous `T` (std::any erasure) | yes (add/remove/hot-swap/enable) | stages must be reconfigured live, config-driven, or A/B-swapped without restart |
| **`PipelineGraph<T>`** | DAG | single homogeneous `T` | no (built before `start`) | fan-out (1→N), fan-in (N→1), conditional/A-B routing |

All three expose the same surface: `start(Dispatcher&)`, `push(T, Context)` → `Task<Result<void>>`, `try_push(...)`, `drain()` → `Task<void>`, `stop()`, and `output()` → `shared_ptr<AsyncChannel<ContextualItem<...>>>`.

### 3.1 `StaticPipeline` + `PipelineBuilder` (`static_pipeline.hpp`)

The type chain is enforced at compile time: each `add<Out>(fn)` requires the previous stage's output type to match the new stage's input. A mismatch is a compile error.

```cpp
#include <qbuem/pipeline/static_pipeline.hpp>
using namespace qbuem;

// stages
Task<Result<Parsed>>   parse  (Raw r,    ActionEnv) { co_return Parsed{r}; }
Task<Result<Enriched>> enrich (Parsed p, ActionEnv) { co_return Enriched{p}; }

auto pipeline = PipelineBuilder<Raw>{}          // or: pipeline_builder<Raw>()
    .add<Parsed>(parse)
    .add<Enriched>(enrich)
    .build();                                   // StaticPipeline<Raw, Enriched>

Dispatcher dispatcher(4);
pipeline.start(dispatcher);

co_await pipeline.push(Raw{...});               // backpressure
bool ok = pipeline.try_push(Raw{...});          // non-blocking

auto out = pipeline.output();                   // AsyncChannel<ContextualItem<Enriched>>
auto item = co_await out->recv();

co_await pipeline.drain();                       // close + wait for workers
```

`add<Out>(fn, cfg)` takes a per-stage `Action<...>::Config` (worker counts, channel cap). State machine: `Created → Starting → Running → Draining → Stopped` (`state()` reports it). `StaticPipeline` is move-only.

**Source/sink wiring** lets you attach external producers/consumers instead of `push()`:

```cpp
auto p = PipelineBuilder<RawOrder, ValidatedOrder>{}
    .with_source(SHMSource<RawOrder>("orders_channel"))      // call BEFORE add()
    .add<ValidatedOrder>(validate)
    .with_sink(MessageBusSink<ValidatedOrder>(bus, "validated")) // AFTER add chain
    .build();
p.start(dispatcher);
```

A *source* type must provide `init() -> Result<void>` and `next() -> Task<std::optional<const Out*>>`; a *sink* must provide `init() -> Result<void>` and `sink(const Out&) -> Task<Result<void>>`. `MessageBusSource`/`MessageBusSink` (§9.2) implement these. See `examples/06-ipc-messaging/ipc_pipeline/` for SHMSource→pipeline→MessageBusSink wiring.

**Gotchas:** `with_source()` must precede `add()`; `with_sink()` follows the `add` chain and precedes `build()`. The builder spawns internal pump coroutines lazily at `start()`. A source whose `init()` fails is silently skipped.

### 3.2 `DynamicPipeline<T>` (`dynamic_pipeline.hpp`)

A linear chain of homogeneous-`T` stages you can mutate at runtime. Type erasure is via `std::any`, so all stages are `T → T`.

```cpp
#include <qbuem/pipeline/dynamic_pipeline.hpp>

DynamicPipeline<int> dp({.default_channel_cap = 256, .default_workers = 2});
dp.add_stage("double", [](int x, ActionEnv) -> Task<Result<int>> { co_return x*2; });
dp.add_stage("addone", [](int x, ActionEnv) -> Task<Result<int>> { co_return x+1; });
dp.start(dispatcher);

co_await dp.push(42);

// live reconfiguration:
dp.hot_swap("addone", [](int x, ActionEnv) -> Task<Result<int>> { co_return x+10; });
dp.set_enabled("double", false);   // disabled stage = pass-through
dp.remove_stage("double");         // bypass + rewire
```

`StageFn` is `std::function<Task<Result<T>>(T, ActionEnv)>`. `add_stage(name, fn, workers=0, chan_cap=0)` (0 = use config defaults). `hot_swap` signals existing workers to stop and installs the new function for next spawn. `set_enabled(name,false)` turns a stage into a pass-through. Channels are auto-rewired so `stage[i].out == stage[i+1].in`.

**When to use over StaticPipeline:** only when you genuinely need runtime mutation (config-driven assembly, live A/B). Otherwise prefer `StaticPipeline` for type-safety and lower overhead. **Gotchas:** all stages must share one type `T`; `push()` on an empty pipeline returns `errc::no_such_process`; `hot_swap`'s `timeout_ms` parameter is currently ignored.

To embed a fully-typed `StaticPipeline<In,Out>` as a single `T→T` dynamic stage where `In==Out`, use `make_dynamic_action` (`dynamic_pipeline.hpp:489`).

### 3.3 `PipelineGraph<T>` (`pipeline_graph.hpp`)

A DAG over homogeneous `T`. Construction is fluent: `node()`, `edge()`/`edge_if()`, `source()`, `sink()`. `start()` topo-sorts (Kahn's algorithm) and returns `false` on a cycle.

```cpp
#include <qbuem/pipeline/pipeline_graph.hpp>

PipelineGraph<LogEntry> graph;
graph.node("ingest",    ingest,    1, 64)   // (name, fn, workers, chan_cap)
     .node("normalize", normalize, 2, 64)
     .node("audit",     audit,     1, 64)
     .edge("ingest", "normalize")           // fan-out: ingest → {normalize, audit}
     .edge("ingest", "audit")
     .source("ingest")
     .sink("normalize")
     .sink("audit");

if (!graph.start(dispatcher)) { /* cycle detected */ }

graph.try_push(LogEntry{"line-0"});
auto merged = graph.output();               // all sink outputs merged here
auto item = co_await merged->recv();
```

Fan-out copies the item to every matching successor; **fan-in** (multiple `sink()` nodes) is merged into one output channel. Conditional routing via `edge_if(from, to, [](const T&){...})` (A/B). `validate()` checks endpoints/sources/sinks exist. `to_dot()` / `to_mermaid()` export the topology for visualization.

See `examples/05-pipeline/fanout/` for a runnable fan-out/fan-in graph (`ingest → {normalize, audit}` with a merged output, polled via `output()->try_recv()`).

**When NOT to use:** for a pure 1→N broadcast at the channel level (no per-branch transform), `DynamicRouter` (§6.6) is lighter. **Gotchas:** the graph must be acyclic; build it fully before `start()` (nodes/edges aren't added live); all nodes share type `T`.

---

## 4. Channels — pick the right one

All channels are EOS-aware: `close()` propagates end-of-stream so `recv()` returns `std::nullopt` after draining and `send()` returns `errc::broken_pipe`. Capacities round up to a power of two.

| Channel | Concurrency | `co_await` send/recv | Allocation | Use when |
|---|---|---|---|---|
| **`AsyncChannel<T>`** | MPMC (Vyukov ring) | yes | heap slots at construction | cross-reactor / multi-producer-consumer — the default everywhere in this layer |
| **`SpscChannel<T>`** | 1 producer, 1 consumer | yes | vector buffer | exactly one writer + one reader; faster than MPMC |
| **`ArenaChannel<T>`** | single reactor, no locks | **no** (sync `push`/`pop`) | one pool alloc, then O(1) free-list | producers+consumers on the *same* reactor thread; zero per-message heap |
| **`PriorityChannel<T>`** | MPMC, 3 levels | yes | 3× `AsyncChannel` | High/Normal/Low ordering needed |

### 4.1 `AsyncChannel<T>` (`async_channel.hpp:59`)

The workhorse. Lock-free `try_send`/`try_recv` (wait-free fast path), `co_await send()`/`recv()` with backpressure, batch `try_recv_batch(span)` / `send_batch(span)`, `size_approx()`, `capacity()`, `is_closed()`. `head_`/`tail_` are on separate cache lines (`alignas(64)`) to avoid false sharing; waiters are resumed on their owning reactor (cross-reactor safe).

```cpp
AsyncChannel<int> ch(1024);
co_await ch.send(42);
auto v = co_await ch.recv();         // std::optional<int>; nullopt = EOS
if (!ch.try_send(7)) { /* full or closed */ }
ch.close();
```

### 4.2 `SpscChannel<T>` (`spsc_channel.hpp:48`)

Lamport SPSC ring. Use only with a single producer and single consumer — there's exactly one waiter slot. Same `send/recv/try_send/try_recv/close` surface as `AsyncChannel`.

### 4.3 `ArenaChannel<T>` (`arena_channel.hpp:77`)

Reactor-local, zero-heap-after-construction queue built on `FixedPoolResource`. **Synchronous only** (`push`/`pop`/`emplace`/`pop_batch`) — no `co_await`. Safe only within a single reactor thread; for cross-reactor use `AsyncChannel`. This is the zero-allocation choice (pillar A) for hot intra-reactor handoff.

```cpp
ArenaChannel<int> ch(256);           // 256 slots heap-allocated ONCE
if (!ch.push(42)) { /* pool full = backpressure */ }
ch.emplace(/*ctor args*/);
if (auto v = ch.pop()) process(*v);
```

### 4.4 `PriorityChannel<T>` (`priority_channel.hpp:51`)

Three internal `AsyncChannel`s (`Priority::High/Normal/Low`); `recv()` drains High→Normal→Low. `send(item, Priority::High)`, `try_send(item, prio)`, `size_approx(Priority)`.

```cpp
PriorityChannel<Job> q(128);                  // per-level capacity
co_await q.send(urgent, Priority::High);
co_await q.send(bulk,   Priority::Low);
auto job = co_await q.recv();                  // High first
```

**Gotcha:** under sustained High traffic, Low can starve — by design.

---

## 5. Resilience wrappers

These wrap a stage function (`std::function<Task<Result<Out>>(In, ActionEnv)>`) and are themselves callables you can drop into any pipeline `add<Out>(...)`. See `examples/07-resilience/`.

### 5.1 `RetryAction<In, Out>` (`retry_policy.hpp:64`)

Retries a stage on failure per `RetryConfig`. Async backoff uses the reactor timer (`Reactor::register_timer`), falling back to a thread sleep only if no reactor is running.

```cpp
RetryConfig cfg{
    .max_attempts = 5,
    .base_delay   = std::chrono::milliseconds{50},
    .max_delay    = std::chrono::seconds{5},
    .strategy     = BackoffStrategy::Jitter,        // Fixed | Exponential | Jitter
    .is_retriable = [](const std::error_code& ec) { return ec != std::errc::invalid_argument; },
};
auto retried = make_retry_action<Req, Resp>(call_upstream, cfg);

auto p = PipelineBuilder<Req>{}.add<Resp>(retried).build();
```

`Exponential`: `base * 2^attempt` capped at `max_delay`. `Jitter` adds up to 10%. Honors `env.stop` between attempts (returns `errc::operation_canceled`). The item is copied per attempt so the original survives retries.

### 5.2 `CircuitBreaker` + `CircuitBreakerAction<In,Out>` (`circuit_breaker.hpp`)

Three-state, **consecutive-failure** breaker (`Closed → Open → HalfOpen → Closed`). Any success in Closed resets the failure streak, so alternating success/failure traffic won't trip it.

```cpp
auto cb = std::make_shared<CircuitBreaker>(CircuitBreakerConfig{
    .failure_threshold = 5,
    .success_threshold = 2,
    .timeout = std::chrono::seconds{30},
    .on_state_change = [](CircuitBreakerState from, CircuitBreakerState to) { /* log */ },
});
CircuitBreakerAction<Req, Resp> guarded(cb, call_upstream);
// guarded(req, env) fast-fails with errc::connection_refused while Open.
```

Share one `CircuitBreaker` (via `shared_ptr`) across stages that hit the same dependency. Compose with retry: wrap the inner call in `CircuitBreakerAction`, then in `RetryAction`, so retries see the breaker.

### 5.3 `DeadLetterQueue<T>` + `DlqAction<In,Out>` (`dead_letter.hpp`)

`DlqAction` retries up to `max_attempts`; on final failure it pushes a `DeadLetter<T>` (item + ctx + error + attempt count + timestamp) into the queue. The queue is thread-safe, bounded (`Config::max_size`, oldest dropped), and supports `peek()`, `drain()`, and async `reprocess(fn, max_n)` (re-enqueues entries that fail again).

```cpp
auto dlq = std::make_shared<DeadLetterQueue<Order>>(
    DeadLetterQueue<Order>::Config{.max_size = 10000});
DlqAction<Order, Receipt> action(process_order, dlq, /*max_attempts=*/3);

// later, retry the failures:
size_t recovered = co_await dlq->reprocess(
    [](Order o, Context) -> Task<Result<void>> { co_return Result<void>{}; });
```

This is the explicit mechanism for capturing failed items — bare pipeline workers drop errored results.

### 5.4 `SagaOrchestrator<T>` (`saga.hpp`)

Sequential distributed transaction with reverse compensation. Each `SagaStep<T,T>` has `name`, `execute` (`Task<Result<T>>(T)`), and `compensate` (`Task<void>(T)`). If step N fails, steps N-1…0 are compensated in reverse; compensation failures are recorded (queryable via `compensation_failures()`) rather than aborting rollback.

```cpp
SagaOrchestrator<Order> saga;
saga.add_step({.name="reserve",
               .execute=[](Order o)->Task<Result<Order>>{ co_return o; },
               .compensate=[](Order o)->Task<void>{ co_return; }})
    .add_step({.name="charge", .execute=charge, .compensate=refund});
auto r = co_await saga.run(order, ctx);   // Result<Order>
```

> Saga is the one place exceptions are caught (in `compensate_steps_`), because compensation must not abort midway. Your `execute`/`compensate` bodies should still prefer returning errors.

### 5.5 `IdempotencyFilter<T>` (`idempotency.hpp`)

De-duplicates at-least-once delivery by reading `IdempotencyKey` from `env.ctx`. `process(item, env)` returns `Result<optional<T>>`: the item on first-seen, `nullopt` on duplicate, and passes through unchanged if no key slot is present. `InMemoryIdempotencyStore` is the default backing store (lazy TTL expiry); implement `IIdempotencyStore` (`set_if_absent`, `get`) for Redis/DB.

```cpp
auto store = std::make_shared<InMemoryIdempotencyStore>();
IdempotencyFilter<Order> filter(store, std::chrono::hours{24});
auto r = co_await filter.process(order, env);
if (!r) co_return std::unexpected(r.error());
if (!r->has_value()) co_return /* skip duplicate */;
auto item = std::move(**r);
```

---

## 6. Throughput shaping & event actions

These manage their own worker loop and channels; lifecycle mirrors `Action`: `start(dispatcher, out)` → `push/try_push` → `drain()`/`stop()`, with `input()`/`output()` accessors.

### 6.1 `BatchAction<In, Out>` (`batch_action.hpp:48`)

Collects up to `max_batch_size` items or until `max_wait_ms` elapses, then calls one batch function. Signature: `Task<Result<std::vector<Out>>>(std::vector<In>, ActionEnv)`. Every output item inherits the **first** input item's `Context`.

```cpp
BatchAction<Row, Ack> inserter(
    [](std::vector<Row> rows, ActionEnv) -> Task<Result<std::vector<Ack>>> {
        // one DB round-trip for the whole batch
        co_return std::vector<Ack>(rows.size());
    },
    {.max_batch_size = 256, .max_wait_ms = 5, .workers = 2, .channel_cap = 4096});
inserter.start(dispatcher, out);
co_await inserter.push(row);
```

Ideal for DB bulk inserts, batched HW dispatch, or batched ML inference. See `examples/05-pipeline/hardware_batching/`.

### 6.2 `WindowedAction<T, Key, Acc, Out>` (`windowed_action.hpp:226`)

Event-time, keyed windowed aggregation with watermarks. Window types: `Tumbling`, `Sliding` (step < size), `Session` (gap timeout). Reads `EventTime` from `ctx` (falls back to wall clock). An input worker accumulates; a ticker advances the watermark and emits windows whose end ≤ watermark.

```cpp
WindowedAction<Click, std::string, uint64_t, std::pair<std::string,uint64_t>> w({
    .type   = WindowType::Tumbling,
    .size   = std::chrono::milliseconds{1000},
    .key_fn = [](const Click& c){ return c.user_id; },
    .acc_fn = [](uint64_t& a, const Click&){ ++a; },
    .emit_fn= [](std::string k, uint64_t a, std::chrono::system_clock::time_point){
                  return std::make_pair(std::move(k), a); },
    .init_acc = 0,
});
w.start(dispatcher, out);
co_await w.push(click, Context{}.put(EventTime{event_ts}));
```

`advance_watermark(Watermark{ts})` injects watermarks from an external source. On EOS all remaining windows flush. `Key` requires `std::hash<Key>`. See `examples/05-pipeline/windowed_action/`.

### 6.3 `DebounceAction<T>` & `ThrottleAction<T>` (`event_actions.hpp`)

`DebounceAction` emits the last item after a silence `gap` (collapses bursts). `ThrottleAction` is a token-bucket rate limiter (`rate_per_sec`, `burst`).

```cpp
DebounceAction<Event> deb({.gap = std::chrono::milliseconds{200}});
ThrottleAction<Event> thr({.rate_per_sec = 1000, .burst = 100});
deb.start(dispatcher, mid); thr.start(dispatcher, out);
```

### 6.5 `ScatterGatherAction<In, SubIn, SubOut, Out>` (`event_actions.hpp:453`)

Splits one input into many `SubIn`, processes them in parallel (batched by `max_parallel`), and gathers `SubOut`s into one `Out`.

```cpp
ScatterGatherAction<Query, Shard, ShardResult, Merged> sg(
    /*scatter*/[](Query q){ return shard(q); },
    /*process*/[](Shard s, ActionEnv) -> Task<Result<ShardResult>> { co_return run(s); },
    /*gather*/ [](Query q, std::vector<ShardResult> rs){ return merge(std::move(rs)); },
    {.max_parallel = 8});
sg.start(dispatcher, out);
```

Only successful `SubOut`s reach `gather` (failed sub-tasks are dropped). See `examples/07-resilience/scatter_gather/`. (There is also a streaming-style `scatter_gather` over `Stream` combinators — see §8.)

### 6.6 `DynamicRouter<T>` (`dynamic_router.hpp:131`)

Channel-level fan-out by predicate, with SIMD-friendly batch evaluation (AVX2/SSE4.2 fast paths, scalar/NEON fallback). Modes: `FirstMatch`, `AllMatch` (fan-out), `LoadBalance` (round-robin, ignores predicates).

```cpp
AsyncChannel<Order> high(1024), normal(1024), dlq(64);
DynamicRouter<Order> router(RoutingMode::AllMatch);
router.add_route("high",   high,   [](const Order& o){ return o.priority > 8; });
router.add_route("normal", normal, [](const Order& o){ return o.priority <= 8; });
router.set_default(dlq);                    // unmatched items

// in a stage:
size_t n = co_await router.route(order, env.stop);   // # channels sent to

auto [routed, dropped] = router.stats("high");
```

Per-route `blocking` flag chooses `co_await send()` (backpressure) vs `try_send()` (drop-on-full). `evaluate_batch(span)` returns per-item bitmasks for offline routing decisions. Channels are referenced non-owning — they must outlive the router. **DynamicRouter vs PipelineGraph:** use the router for pure channel routing with no per-branch transform; use `PipelineGraph` when each branch is a processing node.

---

## 7. Streams (Rx-style)

### 7.1 `Stream<T>` + combinators (`stream.hpp`)

`Stream<T>` is a lazy pull view over an `AsyncChannel<T>`: `co_await stream.next()` yields `std::optional<T>` (`nullopt` = EOS). Combinators use `operator|` and spawn detached pump coroutines (a reactor must be running to drive them).

```cpp
auto [stream, chan] = make_stream<int>();     // {Stream<int>, shared_ptr<AsyncChannel<int>>}
// feed `chan` from a producer coroutine, then:
auto result = stream
    | stream_map([](int x) -> Task<Result<int>> { co_return x * 2; })
    | stream_filter([](int x) { return x > 5; })
    | stream_chunk(10);                        // Stream<std::vector<int>>
auto batch = co_await result.next();
```

Operators: `stream_map` (`T → Task<Result<U>>`), `stream_filter`, `stream_chunk(n)` (→ `vector<T>`), `stream_take_while`, `stream_scan(init, fn)` (stateful), `stream_map_filter(map, pred)` (fused — no intermediate channel), `stream_flat_map` (`T → Stream<U>`), `stream_zip(a, b)`, `stream_merge(vec)` / `stream_merge(a, b)`, plus `Stream::tee()`.

**When NOT to use streams:** for high-throughput multi-worker stages, the `Action`/pipeline path autoscales and is more efficient. Streams shine for single-consumer transform chains and Rx-style composition. (For throttle/debounce/windowing as pipeline stages, use `ThrottleAction`/`DebounceAction` (§6.3) and `WindowedAction` (§6.2).)

---

## 8. Structured concurrency — `TaskGroup` (`task_group.hpp`)

Nursery pattern: spawn children, then `co_await join()` — the parent won't return until all children finish. If a child fails, the rest are cancelled (`stop_token()`) and the first error propagates.

```cpp
TaskGroup tg;
tg.spawn(do_work_a());                 // Task<Result<void>>
tg.spawn(do_work_b());
auto r = co_await tg.join();           // Result<void>

TaskGroup tg2;
tg2.spawn<int>(compute_a());           // Task<Result<int>>
tg2.spawn<int>(compute_b());
auto results = co_await tg2.join_all<int>();   // Result<std::vector<int>>
```

`cancel()` requests stop on all children; `stop_token()` hands children the cancellation signal. Move-only. See `examples/08-observability/task_group/`.

---

## 9. Wiring & messaging

### 9.1 `ServiceRegistry` (`service_registry.hpp`) — scoped DI

Hierarchical DI container (Global → Pipeline → Action). `get<T>()` = weak (nullptr if absent); `require<T>()` = strong (calls `std::terminate()` if absent — fail-fast); `get_or_create<T>()` for default-constructible types. `register_singleton<T>(ptr)` / `register_factory<T>(fn)` (lazy, cached). Reachable inside any stage via `env.registry`.

```cpp
global_registry().register_singleton<ILogger>(make_logger());
ServiceRegistry pipe_reg(&global_registry());
pipe_reg.register_singleton<IMetrics>(make_metrics());

// inside a stage:
auto log = env.registry->require<ILogger>();   // terminates if missing
auto m   = env.registry->get<IMetrics>();       // nullptr if missing
```

Pass a registry into a pipeline via the relevant `Config::registry` (e.g. `Action::Config`, `DynamicPipeline::Config`); workers default to `global_registry()` when none is set.

### 9.2 `MessageBus` (`message_bus.hpp`) — in-process pub/sub

Topic-based, type-erased (`std::any`) pub/sub with RAII subscriptions. Patterns: unary handler (`subscribe`), server-stream (`subscribe_stream<T>` → typed `AsyncChannel<T>`), client-stream accumulate (`subscribe_accumulate<In,Out>`). `publish<T>()` fans out to all subscribers with backpressure; `try_publish<T>()` drops on full.

```cpp
MessageBus bus;
bus.start(dispatcher);
auto stream_ch = bus.subscribe_stream<Order>("orders");      // AsyncChannel<Order>
co_await bus.publish("orders", Order{...});
auto sub = bus.subscribe("alerts", [](MessageBus::Msg m, Context) -> Task<Result<void>> {
    auto& a = std::any_cast<Alert&>(m); co_return Result<void>{};
});  // sub auto-unsubscribes on destruction
```

**Pipeline bridges:** `MessageBusSource<T>` (topic → pipeline head, via `with_source`) and `MessageBusSink<T>` (pipeline tail → topic, via `with_sink`). See `examples/06-ipc-messaging/message_bus/` and the multi-scenario `examples/06-ipc-messaging/ipc_pipeline/`.

> Cross-process pub/sub uses `SHMBus` (in the `shm/` module), not `MessageBus`. `MessageBus` is in-process only.

---

## 10. Operations / observability / advanced

### 10.1 `BackpressureMonitor` (`backpressure_monitor.hpp`)

Per-stage telemetry via cache-line-aligned atomics. `register_stage(name, capacity)` once, then on the hot path `stage(name).record_enqueue()` / `.record_dequeue(latency_ns, bytes)` / `.record_error()`. Snapshots (`StagePressure` with p50/p99/p99.9, fill ratio, throughput) via `snapshot(name)` / `all_snapshots()`; threshold alerts via `check_alerts(BackpressureAlert)`.

```cpp
BackpressureMonitor mon;
mon.register_stage("parse", 256);
auto& m = mon.stage("parse");
m.record_enqueue();
auto t0 = StageMetrics::now_ns();
/* ...process... */
m.record_dequeue(StageMetrics::now_ns() - t0);
for (auto& s : mon.all_snapshots())
    std::println("{} depth={}/{} p99={}ns", s.name, s.queue_depth, s.capacity, s.latency_p99_ns);
```

Snapshot/alert calls are cold-path — run them from a monitoring thread, not the reactor. See `examples/05-pipeline/backpressure_monitor/`.

### 10.2 SLO tracking (`slo.hpp`)

`SloConfig` (p99/p999 targets, `error_budget`, `on_violation`) can be attached to an `Action` via `Action::Config::slo`. Standalone: `ErrorBudgetTracker` records `record_success(latency)` / `record_error()`, exposes `error_rate()`, `budget_exhausted()`, and `check_slo()` (fires the callback on breach). `LatencyHistogram` gives `p99()`/`p999()` over a 1024-sample rolling window. For event hooks, implement `PipelineObserver` (`observability.hpp`).

```cpp
Action<Req,Resp> a(handle, {.slo = SloConfig{
    .p99_target  = std::chrono::microseconds{5'000},
    .error_budget = 0.001,
    .on_violation = [](std::string_view name){ alert(name); }}});
```

### 10.3 `PipelineObserver` (`observability.hpp`)

Event-hook interface: `on_item_start/on_item_done/on_error/on_scale_event/on_state_change/on_dlq_item/on_circuit_open/on_circuit_close`. `LoggingObserver` is a ready-made impl; `NoopObserver` is the safe default. `ActionMetrics`/`PipelineMetrics`/`HistogramMetrics` provide counters.

### 10.4 `CanaryRouter<T>` (`canary.hpp`)

Splits traffic between a stable and a canary pipeline by percentage, with automatic gradual rollout and metric-based rollback.

```cpp
CanaryRouter<Order> router;
router.set_stable([&](Order o){ return stable.try_push(o); })
      .set_canary([&](Order o){ return canary.try_push(o); });
router.push(order);                            // routed by current %
co_await router.start_gradual_rollout({        // background coroutine
    .steps = {1, 5, 25, 100},
    .step_duration = std::chrono::seconds{60},
    .max_error_delta = 0.01, .max_latency_ratio = 1.5,
    .on_rollback = [](std::string_view why){ /* ... */ }});
```

`set_canary_percent(pct)` and `rollback_to_stable()` give manual control. See `examples/07-resilience/canary/`.

### 10.5 `CheckpointedPipeline<T>` (`checkpoint.hpp`)

A `DynamicPipeline<T>` plus periodic state snapshots to an `ICheckpointStore` (`InMemoryCheckpointStore` provided). `enable_checkpoint(every_t, every_n)`, `push_counted()` (auto-saves on count/time), `save_checkpoint()`, `resume_from_checkpoint()`. Access the inner pipeline via `pipeline()` to `add_stage`/`start`.

```cpp
auto store = std::make_shared<InMemoryCheckpointStore>();
CheckpointedPipeline<int> cp("etl", store);
cp.pipeline().add_stage("double", [](int x, ActionEnv)->Task<Result<int>>{co_return x*2;});
cp.enable_checkpoint(std::chrono::seconds{30}, 1000);
cp.pipeline().start(dispatcher);
co_await cp.push_counted(item);
```

See `examples/07-resilience/checkpoint/` and `examples/07-resilience/idempotency_slo/`.

### 10.6 health (`health.hpp`)

`PipelineVersion` (+ `compatible_with`), `ActionHealth`, `HealthStatus { HEALTHY, DEGRADED, UNHEALTHY }`, `PipelineHealth::recompute()`, `HealthRegistry`, `PipelineVersionRegistry`, and `GraphTopologyExporter`. See `examples/05-pipeline/observer_health/`.

---

## 11. Cross-cutting rules (pillar compliance)

- **Zero allocation (pillar A):** stage bodies on the hot path must avoid `new`/`std::vector`/`std::string`/`std::format`/`std::function`/`std::shared_ptr`. Use `WorkerLocal<T>` scratch, `Arena`, and `ArenaChannel` for intra-reactor handoff. `Context::put` is O(1) but does allocate a node — build contexts at the edge, not per-item in tight loops.
- **Zero copy (pillar C):** prefer `const&`/`std::span`/`std::string_view` in stage signatures; `std::move` ownership out of a stage. Fan-out in `PipelineGraph`/`DynamicRouter` must copy the value per branch (unavoidable), but `Context` copies are O(1) shared-pointer prepends.
- **Zero latency (pillar L):** check `env.stop.stop_requested()` before each `co_await`; never block a reactor thread (`RetryAction` uses the reactor timer, falling back to a thread sleep only when no reactor is present — keep one running).
- **No exceptions:** return `std::unexpected(...)`. The one intentional, cold-path exception is `SagaOrchestrator` catching inside compensation.
- **Lifetime:** channels passed to `DynamicRouter` / `MessageBusSource` must outlive their consumers. `IOVec`/`scattered_span` (io module) lifetime rules apply when bridging to sockets.
- **Threading:** `AsyncChannel`/`PriorityChannel`/`SpscChannel` resume waiters on their owning reactor (cross-reactor safe). `ArenaChannel` is single-reactor only. Never call `handle.resume()` across reactor threads — use `reactor->post(...)`.
