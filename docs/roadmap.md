# qbuem-stack Roadmap

**Current released version: v1.6.0.** This document describes where the project
actually is and where it is going. For per-release detail see
[`CHANGELOG.md`](../CHANGELOG.md); for what is stable vs experimental see the
[feature-maturity matrix in the README](../README.md#feature-maturity).

> **Honesty rule.** This roadmap lists only things that exist (under "Where we
> are") or that are concretely planned (under "Where we're going"). It does **not**
> describe aspirational features as done. Out-of-scope items are named explicitly.

---

## Where we are (v1.6.0)

A focused, zero-dependency C++23 library for WAS / IPC / real-time simulation /
pipelines, validated by being dogfooded in a real application (the `qbuem-game`
server, which pins a released tag and exercises core, `WsServer`,
`TickScheduler`, pipeline, SHM, crypto and the spatial indexes).

**Production-ready (Stable):** async core (reactor + coroutines + dispatcher +
offload), zero-alloc memory & timing (`Arena`, pools, `TimerWheel`,
`TickLoop`/`TickScheduler`), HTTP/1.1 `App` server + `fetch` client,
`WsServer`, the pipeline engine, SHM IPC, sockets, scatter-gather I/O, crypto +
JWT, spatial indexes, middleware, tracing, config.

**Experimental:** the `db` layer (interfaces + connection pool; **no bundled
driver** — you implement `IConnection`).

**Codec-only (building blocks, not turnkey servers):** `Http2Handler` (HPACK
static table + framing; no flow control, no wired transport) and `GrpcHandler`
(message framing + dispatch; no HTTP/2 transport). Their DoS surfaces are
hardened, but neither is reachable from the `App` umbrella yet.

**Verification baseline:** CI builds + runs tests **and** examples on Linux
x86_64, Linux ARM64, and macOS aarch64; runs ASan + UBSan + ThreadSanitizer +
fuzz smoke; enforces the zero-dependency rule and clang-tidy.

---

## Where we're going

The theme for the 1.x line is **depth and trust, not breadth.** The library is
already wide; the work is making the parts people actually use production-grade,
proving the performance claims, and keeping the docs honest.

### v1.6 — Truth & Trust  *(this release)*
- [x] Docs rewritten to match the implementation; fictional/out-of-scope docs removed.
- [x] Feature-maturity matrix; `CHANGELOG.md`; honest `version.hpp` history.

### v1.7 — Depth on the core path  *(done)*
Make the heavily-used path production-grade and ergonomic.
- [x] `App` high-level surface for WebSocket — `app.ws("/path", handlers)` serves
      WS on the same port as HTTP, no hand-wired upgrade callback (v1.7.0).
- [x] `MessageBus` publish without per-message handler copies — RCU snapshot
      (v1.7.1). *(Persistent reactor registration for the awaiter fast path
      remains to evaluate — deferred to v2.0 identity work.)*
- [x] Per-subsystem [production checklist](./production-checklist.md) (real
      limits, timeouts, back-pressure, teardown) for the Stable modules.

### v1.8 — Proof  *(in progress)*
Turn performance *targets* into *measured, tracked* numbers.
- [x] Reproducible benchmark results with the measurement environment
      ([`bench/RESULTS.md`](../bench/RESULTS.md), Apple M1 Pro baseline) + a CI
      `bench-smoke` job that builds and runs every benchmark (regressions/crashes
      surface; thresholds are not asserted since they are hardware-dependent).
- [x] `qbuem-game` documented as the flagship real-world consumer (pins a
      released tag; exercises core/WsServer/TickScheduler/pipeline/SHM/crypto).
- [ ] At least one second reference application in a different domain.
- [ ] Add more machine baselines to `bench/RESULTS.md` (server x86).

### v2.0 — Identity  *(decision point, not a feature dump)*
Only after 1.7–1.8, and informed by what adopters actually ask for, decide the
library's identity:
- (a) finish `Http2Handler`/`GrpcHandler` into real, wired servers; or
- (b) go deep on the real-time / game-server niche; or
- (c) stay a focused "async core + HTTP/1.1 + WebSocket + pipeline" library.

A `2.0` major is also the moment to make any breaking API cleanups.

---

## Explicitly out of scope

These are **not** planned for the zero-dependency core, because they require
third-party libraries or special hardware (which would break the core contract)
or an unsupported platform:

- **TLS / HTTPS termination** (would need a crypto library for the handshake).
  The `fetch` client is HTTP-only; terminate TLS at a reverse proxy.
- **RDMA, PCIe/VFIO, NVMe passthrough, eBPF, AF_XDP** — specialized hardware
  paths.
- **Windows** — POSIX only (Linux + macOS).

If a future need is strong enough to justify an optional, separately-built module
that breaks zero-dependency *for that module only*, it will be proposed
explicitly here first — not shipped silently.
