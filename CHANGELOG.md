# Changelog

All notable changes to **qbuem-stack** are documented here. Only released,
git-tagged versions are listed. The format follows
[Keep a Changelog](https://keepachangelog.com/), and the project adheres to
[Semantic Versioning](https://semver.org/) (`MAJOR.MINOR.PATCH`).

The 1.0.0 release is the first public stable baseline; everything before it was
pre-release internal iteration and is not tracked here.

---

## [2.0.0] — Remove redundant pipeline surface (breaking)

**BREAKING.** Deletes the over-engineered / redundant pipeline APIs outright
(rather than deprecating them). No additions; consumers pinned to 1.x are
unaffected (this is a major bump).

### Removed
- `pipeline/pipeline_factory.hpp` (`PipelineFactory`) — use `StaticPipeline` or
  `PipelineGraph` built in code.
- `pipeline/migration.hpp` (`MigrationAction`, `DlqReprocessor`) — use a plain
  `Action<Old,New>` transform stage; `DeadLetterQueue` for re-drive.
- `pipeline/stream_ops.hpp` (`stream_throttle`/`stream_debounce`/
  `stream_tumbling_window`) — use `ThrottleAction`/`DebounceAction`
  (`event_actions.hpp`) and `WindowedAction`.
- `pipeline/stateful_window.hpp` (`StatefulWindow`, `make_tumbling_window`,
  `make_counting_window`) — use `WindowedAction` + `TumblingWindow`.
- `pipeline/subpipeline_action.hpp` (standalone) — duplicate of the
  `SubpipelineAction` in `message_bus.hpp`; that one remains. Resolves a latent
  ODR hazard.
- `SloObserver` / `LoggingSloObserver` from `pipeline/slo.hpp` (zero consumers) —
  use `PipelineObserver` (`observability.hpp`). The rest of `slo.hpp`
  (`SloConfig`, `LatencyHistogram`, `ErrorBudgetTracker`) is unchanged.
- The four corresponding `examples/05-pipeline/` demos (factory, stream_ops,
  stateful_window, subpipeline_migration). Canonical examples remain (`fanout`,
  `windowed_action`, `07-resilience/resilience`).

The umbrella `qbuem_stack.hpp` no longer pulls `stateful_window.hpp`. See
`docs/consolidation.md`. Verified: macOS Release/ASan/UBSan/TSan, gcc 13.3
`-Werror`, clang-tidy-18.

---

## [1.9.0] — SaaS readiness + v1 finalization

Backwards-compatible (additive). This release wraps up the **v1** line: it is
SaaS-ready behind an edge TLS terminator, and subsequent changes are hotfix-only.
All new surface is zero-dependency.

### Added
- **External-IdP auth (RS256 / JWKS).** `middleware/rs256_verifier.hpp`:
  `parse_jwks()` + `Rs256JwtVerifier : ITokenVerifier` — `alg`-pinned (rejects
  `none`/`HS256` downgrade), `kid` selection, `exp`/`nbf`, and optional
  `expect_issuer()`/`expect_audience()` (RFC 8725). Drops into `bearer_auth()`.
- **Native RSA verify.** `crypto/rsa.hpp`: zero-dependency
  `rsa_pkcs1_v15_sha256_verify` (RFC 8017), verify-only.
- **Per-tenant quota.** `middleware/quota.hpp`: fixed-window request budget keyed
  by tenant (`X-Auth-Sub`/`X-Tenant-Id`), plan-tier overrides, `X-Quota-*` headers.
- **OTLP/JSON span export.** `tracing/otlp_exporter.hpp`: `encode_otlp_traces_json`
  + `OtlpHttpExporter` (background flush thread, injected transport) behind the
  `SpanExporter` port.
- **HTTP/2 SETTINGS negotiation + flow control** (RFC 7540 §6.5/§6.9) in
  `Http2Handler`; `send_data` enforces the send window. (Still Experimental /
  not socket-wired.)
- `Hs256JwtVerifier` alias; `examples/12-saas/` (RS256 auth, quota, OTLP).

### Changed
- The umbrella `qbuem_stack.hpp` now exposes the SaaS surface (middleware: auth,
  quota, rate-limit, CORS, security, request-id; `crypto/rsa`; OTLP exporter) —
  previously no `middleware/*` was reachable from the umbrella.
- README maturity matrix updated (RS256/JWKS, quota, OTLP, h2 flow control).

### Deprecated
- `SloObserver` / `LoggingSloObserver` (zero consumers) — use `PipelineObserver`.
  See `docs/consolidation.md` for the full canonical-vs-redundant map and the v2
  removal plan (`PipelineFactory`, `migration`, duplicate windowing/stream ops).

### Fixed
- `base64.hpp` `-Werror` unused-variable/parameter in the AVX2 stub + non-NEON
  `encode_impl` path (surfaced once the umbrella pulled it into the gcc library TU).

---

## [1.8.2] — Correctness + CI hardening

No API change.

### Fixed
- **`App::ws()` upgrade handoff use-after-free (kqueue/epoll).** When a WebSocket
  upgrade was handed off to the per-reactor `WsServer`, the App cancelled its own
  read registration (`unregister_event`) before calling `serve_connection`. On
  kqueue/epoll that destroys the currently-executing read-callback closure
  **synchronously**, releasing its captured `ConnCtx` `shared_ptr`; when that was
  the last reference, `ctx->buf` — the backing store for the request's header
  `string_view`s — was freed before the `WsServer` read `Sec-WebSocket-Key` /
  `Sec-WebSocket-Version`, producing a spurious **`400 Bad Request`** (observed
  on macOS; latent on Linux). Fixed by pinning `ConnCtx` with a local strong
  reference across the handoff and **deferring `ctx->buf.erase()`** until after
  the upgrade path is ruled out (so the header views stay valid through
  `serve_connection`). Verified end-to-end (HTTP + WS on one port) on macOS
  (kqueue, Release/ASan/UBSan/TSan) and Linux (io_uring, GCC 13).

### Changed (CI / tooling — no library impact)
- **clang-tidy** is green again: the six `modernize-avoid-c-arrays` findings in
  `ws_server.hpp`, `websocket_handler.hpp`, and `shm_bus.hpp` are converted from
  C-style arrays to `std::array`.
- **Fuzz smoke** links again: the `http_parser` target no longer pulls in
  `src/http/response.cpp` (which transitively referenced `Reactor::current()`
  from the SSE write awaiter); the parser fuzzer only needs `parser.cpp` +
  `request.cpp`.
- **Benchmark job** no longer fails the run when a single benchmark exceeds the
  shared-runner time budget: each benchmark gets a 240 s timeout that emits a CI
  warning instead of a hard failure (a crash still fails the job).

---

## [1.8.1] — CI publishes benchmark numbers (x86_64 + aarch64)

No API change.

### Changed
- The CI benchmark job is now a **matrix** (`linux-x86_64` + `linux-aarch64`) that
  **publishes the measured numbers**: full per-benchmark output is written to the
  run's **Job Summary**, printed in the log, and uploaded as `benchmarks-<arch>`
  **artifacts** — so current per-architecture **server** baselines are always
  visible on the Actions run page (alongside the local M1 baseline in
  `bench/RESULTS.md`). Still a build+run health gate; hardware-dependent
  thresholds are not asserted.

### Verification
Final production-readiness sweep on the Stable surface: `-Werror` (Debug +
Release), `ctest` green, full **ASan + UBSan** and **ThreadSanitizer** suites
green. See [`docs/production-checklist.md`](docs/production-checklist.md).

[1.8.1]: https://github.com/qbuem/qbuem-stack/releases/tag/v1.8.1

## [1.8.0] — Proof: committed benchmarks + CI smoke + production checklist

No API change — turns performance *targets* into *measured, tracked* numbers and
adds operational docs.

### Added
- **`bench/RESULTS.md`** — real, reproducible benchmark numbers with the exact
  measurement environment (Apple M1 Pro baseline) and a how-to-reproduce; the
  README perf table now shows these measured numbers (not unsourced targets).
- **CI `bench-smoke` job** — builds every benchmark `Release` and runs it, so a
  crash/hang/build-break surfaces (hardware-dependent thresholds are not asserted).
- **`docs/production-checklist.md`** — real resource limits, timeouts,
  back-pressure behavior, and shutdown semantics for the Stable modules
  (completes the v1.7 milestone), plus a pre-deploy checklist.
- README documents `qbuem-game` as the flagship real-world consumer.

[1.8.0]: https://github.com/qbuem/qbuem-stack/releases/tag/v1.8.0

## [1.7.1] — MessageBus zero-copy publish fast path

### Changed
- **`MessageBus`** holds its per-topic subscriber list as an immutable RCU
  snapshot (`shared_ptr<const vector<Sub>>`). `publish` / `try_publish` now take
  an **O(1) snapshot** (one refcount bump) and iterate it without holding the
  lock across `co_await` — eliminating the previous per-message copy of every
  subscriber's `std::function` into two freshly-allocated vectors. Writers
  (`subscribe`/`unsubscribe`) copy-on-write. No API change; verified race-free
  under ThreadSanitizer (concurrent publish + subscribe/unsubscribe).

[1.7.1]: https://github.com/qbuem/qbuem-stack/releases/tag/v1.7.1

## [1.7.0] — `App::ws()` — WebSocket on the same App/port

### Added
- **`App::ws(path, handlers)`**: register a high-level WebSocket endpoint that is
  served on the **same port** as the App's HTTP routes — no hand-wiring of the
  upgrade handshake. A matching `Upgrade: websocket` request is upgraded and
  driven by a non-blocking `WsServer` (`on_open`/`on_message`/`on_close`/
  `on_error`, back-pressured send, heartbeat, fragmentation reassembly).
  - Threading: each connection is served on the reactor thread that accepted it,
    via a per-reactor `WsServer` for the route (shared-nothing → callbacks are
    race-free). Rooms/broadcast are therefore **per-reactor**; for whole-server
    broadcast run the App single-reactor (`App{1}`) or use a standalone
    `WsServer`. Verified end-to-end (HTTP + WS on one port) under ASan/UBSan.

[1.7.0]: https://github.com/qbuem/qbuem-stack/releases/tag/v1.7.0

## [1.6.0] — Documentation truth pass

No API or behavior change — a documentation-only release that makes the docs
match the actual implementation.

### Changed
- Rewrote `README.md` and `docs/roadmap.md` to describe only what exists at this
  version; added a **feature-maturity matrix** (Stable / Experimental /
  Codec-only) so consumers know what to build on.
- Cleaned `version.hpp` history to list only real, tagged releases.
- Added this `CHANGELOG.md`.

### Removed
- Docs describing features that are **not implemented** (TLS, PCIe/VFIO, RDMA,
  NVMe passthrough, eBPF, AF_XDP, Windows/RIO) or that were future proposals /
  historical strategy notes. These are out of scope for the zero-dependency core.

---

## [1.5.0] — Precise fixed-timestep ticking

### Added
- **`TickLoop`** (`core/tick_loop.hpp`): drift-free, absolute-deadline tick
  scheduler with deterministic catch-up of missed ticks (bounded by
  `max_catchup`; a `f(seed, tick)` simulation never silently skips), and
  zero-allocation jitter + work latency histograms (p50/p99/p99.9) readable from
  a monitoring thread. Two drive modes: `advance(now)` (reactor coroutine /
  manual / synthetic-clock testable) and `run_pinned()` (dedicated thread,
  nanosleep + busy-spin for sub-millisecond loops).
- **`TickScheduler`** (`core/tick_scheduler.hpp`): high-level multi-rate systems
  (`every`/`phase`/`order`), deterministic `(seed, tick)` splitmix64 RNG,
  pause / time-scale / single-step + render-interpolation `alpha()`, and
  per-system work metrics + an overrun watchdog.

### Fixed
- Data race in the cross-thread tick `stats()` path (now atomic-only). Verified
  under ThreadSanitizer + ASan + UBSan.

---

## [1.4.1] — HTTP server-push streaming

### Added
- HTTP SSE / chunked **flush-on-suspend**: a handler can flush bytes
  mid-coroutine (before it returns) for true server-push / streaming responses.

---

## [1.4.0] — Object spatial index

### Added
- **`SpatialGrid<T>`** (`buf/spatial_grid.hpp`): a uniform-grid object spatial
  index for Area-of-Interest / broad-phase neighbor queries — `insert` / `move`
  / `remove` and query-in-radius.

---

## [1.3.0] — Generation pool

### Added
- **`GenerationPool`** (`buf/generation_pool.hpp`): ABA-safe, generation-tagged
  object pool with `emplace` / `destroy` / `for_each_live` for stable-handle
  entity storage.

---

## [1.2.0] — Misuse-resistant crypto + ergonomics

### Added
- **`crypto/secretbox.hpp`**: `seal_easy` / `open_easy` (AEAD with an
  auto-generated random nonce — removes the nonce-reuse footgun) and
  `password_hash` / `verify_password` (PBKDF2 with a self-describing PHC string).
- **`crypto/jwt.hpp`**: zero-dependency HS256 `encode_jwt_hs256` /
  `verify_jwt_hs256` with a strict `alg` check (rejects `alg=none` and
  asymmetric downgrade) + constant-time signature compare.
- **`middleware/jwt_verifier.hpp`**: `HmacJwtVerifier` — drop-in JWT auth (no
  OpenSSL needed).
- **`io/buffered_reader.hpp`**: `BufferedReader` with `read_until` / `read_line`.
- **`TcpStream::write_all` / `read_exact`** — short-I/O-safe helpers.

### Fixed
- App write paths (`write_all` / `writev_response` / `send_file_body`) no longer
  truncate responses on `EAGAIN` (accepted sockets are non-blocking); macOS
  `sendfile` no longer busy-spins.
- `CircuitBreaker` Closed-state record path is now lock-free.
- Bounded the DNS resolver's concurrent thread count.

---

## [1.1.0] — High-level WebSocket server + high-concurrency hardening

### Added
- **`WsServer` / `WsConnection`** (`server/ws_server.hpp`): a non-blocking,
  reactor-driven, high-concurrency WebSocket server — connection registry, named
  rooms + broadcast, per-connection context, bounded back-pressure send queue,
  heartbeat ping/pong dead-peer detection, fragmentation reassembly, strict
  RFC 6455 framing, UTF-8 validation, close handshake, and a pre-upgrade
  slowloris timeout. Built atop the existing `WebSocketHandler` codec.

### Fixed / Hardened (high-concurrency audit)
- gRPC length-prefix 64-bit overflow guard.
- HTTP/2 DoS limits: Rapid Reset (stream-erase + RST rate-limit),
  `MAX_CONCURRENT_STREAMS`, DATA/CONTINUATION caps, HPACK varint overflow.
- RUDP reorder-window bound; PBKDF2 iteration cap; `SHMBus` topic-name
  use-after-free fix; `Arena` size-overflow guard.
- `TimerWheel::next_expiry_ms()` is now O(1) (was O(active timers) per poll).
- Idempotency store capacity bound; pipeline stream-op busy-spin removal.

---

## [1.0.0] — First public stable release

The consolidated, first tagged/published baseline (SemVer 1.x).

### Highlights
- **Async core**: C++23 coroutine `Task<T>`, per-core reactor
  (`io_uring`/`epoll`/`kqueue`), multi-core `Dispatcher` with graceful drain,
  `OffloadPool` for blocking work, zero-allocation `Arena` /
  `FixedPoolResource`, `TimerWheel`, `MicroTicker`.
- **HTTP/1.1 `App` server**: SIMD parser, radix-tree router, middleware,
  keep-alive, `serve_static`, `sendfile`, health/metrics endpoints; curl-free
  `fetch` client.
- **Pipelines**: Static / Dynamic (hot-swap) / Graph (DAG), channels, batching,
  windows, resilience (retry / circuit breaker / DLQ / saga / canary).
- **IPC**: `SHMChannel`, `SHMBus`, futex sync — zero-copy cross-process.
- **Crypto**: SHA/HMAC/HKDF/PBKDF2/ChaCha20-Poly1305/AES-GCM/Base64/CSPRNG,
  SIMD JWT parser; opt-in `QBUEM_ENABLE_NATIVE_CRYPTO` hardware SHA-2/AES.
- **Net / IO / buffers / middleware / tracing / config**: TCP/UDP/Unix sockets,
  scatter-gather I/O, spatial bitsets, HTTP middleware, W3C tracing, config +
  `Secret<T>`.

### Hardened (2026-06 audit)
- Full TSan / ASan / UBSan CI across Linux x86_64, Linux ARM64, macOS aarch64.
- Pipeline / action worker-coroutine lifetime use-after-free fixes.
- SHM attacker-trust validation; misuse-resistant AEAD.

[1.6.0]: https://github.com/qbuem/qbuem-stack/releases/tag/v1.6.0
[1.5.0]: https://github.com/qbuem/qbuem-stack/releases/tag/v1.5.0
[1.4.1]: https://github.com/qbuem/qbuem-stack/releases/tag/v1.4.1
[1.4.0]: https://github.com/qbuem/qbuem-stack/releases/tag/v1.4.0
[1.3.0]: https://github.com/qbuem/qbuem-stack/releases/tag/v1.3.0
[1.2.0]: https://github.com/qbuem/qbuem-stack/releases/tag/v1.2.0
[1.1.0]: https://github.com/qbuem/qbuem-stack/releases/tag/v1.1.0
[1.0.0]: https://github.com/qbuem/qbuem-stack/releases/tag/v1.0.0
