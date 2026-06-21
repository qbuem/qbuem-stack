# Production Checklist

Practical guidance for running the **Stable** parts of qbuem-stack in production:
the built-in resource limits, timeouts, back-pressure behavior, and shutdown
semantics — with the **actual default values from the code** and whether each is
configurable. Use this to decide your tuning and to know what protects you out of
the box.

> Scope: the modules marked **Stable** in the [feature-maturity matrix](../README.md#feature-maturity).
> Every value below is the real constant at this version — grep the cited symbol
> to confirm.

---

## HTTP/1.1 server (`App`)

| Concern | Default | Configurable? | Source |
|---|---|---|---|
| Max request **body** | 1 MiB → `413` | no (compile-time) | `HttpParser::MAX_BODY_SIZE` |
| Max request **header** section | 8 KiB → `400` | no (compile-time) | `HttpParser::MAX_HEADER_SIZE` |
| **Idle** timeout (between requests) | 30 s → close | no (compile-time) | `ConnCtx::IDLE_TIMEOUT_MS` |
| **Read** timeout (slowloris: time to finish one request) | 10 s → `408` | no (compile-time) | `ConnCtx::READ_TIMEOUT_MS` |
| Max requests per keep-alive connection | 100 → close | no (compile-time) | `ConnCtx::MAX_REQUESTS` |
| Max concurrent connections | unlimited (0) → `503` when set | **yes** — `app.set_max_connections(n)` | `App::set_max_connections` |
| Write-stall budget (slow client, full send buffer) | 5 s, then drop the connection | no (compile-time) | `kWriteStallBudgetMs` |

**Back-pressure / slow clients.** Accepted sockets are non-blocking. When the
kernel send buffer fills, the write path waits up to the 5 s stall budget for the
socket to drain (it does **not** truncate the response — fixed in v1.2.0); past
that, the connection is dropped. Path-traversal (`../`, percent-encoded variants)
is rejected with `400`.

**Teardown.** `app.stop(drain_timeout_ms = 5000)` flips the readiness probe to
`503`, stops accepting new connections, and drains in-flight ones before
returning. Wire it to `SIGTERM`/`SIGINT` (the App installs these handlers in
`listen()`).

**Deploy notes.** Run behind a reverse proxy for **TLS** (there is no TLS in the
core — see the maturity matrix). The proxy should also enforce any
larger-than-default body limits you actually want, since the 1 MiB body cap is
compile-time.

---

## WebSocket (`WsServer`, and `App::ws()`)

All knobs live in `WsServerConfig` and are **configurable** when you construct a
standalone `WsServer{handlers, cfg}`. (`App::ws()` uses the defaults.)

| Concern | Default | Source |
|---|---|---|
| Max reassembled **message** size | 16 MiB → close `1009` | `WsServerConfig::max_message_size` |
| Per-connection **send queue** cap (back-pressure) | 8 MiB → drop/close `1013` | `WsServerConfig::max_send_queue` |
| **Heartbeat** ping interval | 30 s (0 disables) | `WsServerConfig::ping_interval_ms` |
| **Pong** deadline (dead-peer detection) | 10 s → close `1001` | `WsServerConfig::pong_timeout_ms` |
| Max concurrent connections | 65 536 → reject | `WsServerConfig::max_connections` |
| **Handshake** (pre-upgrade slowloris) timeout | 10 s → close | `WsServerConfig::handshake_timeout_ms` |
| UTF-8 validation of text frames | on | `WsServerConfig::validate_utf8` |
| Single inbound frame cap (codec DoS guard) | 16 MiB → reject | `kMaxFramePayload` (websocket_handler) |

**Back-pressure policy.** A connection whose send queue exceeds `max_send_queue`
is **dropped** (closed `1013`), not buffered without bound — the correct policy
for real-time servers. Strict RFC 6455 framing is enforced (RSV/opcode/mask,
control-frame size, fragmentation reassembly).

**Threading.** `WsServer` is single-reactor / shared-nothing — its registry and
rooms are not locked. Scale across cores with one `WsServer` per reactor behind
`SO_REUSEPORT`. With `App::ws()` this is automatic, but rooms/broadcast are then
**per-reactor** (see `App::ws()` docs).

**Teardown.** `~WsServer` hard-terminates all connections on its reactor thread.
Destroy it on its reactor thread, before the reactor.

---

## HTTP/2 & gRPC handlers — **codec-only**

`Http2Handler` / `GrpcHandler` are **not turnkey servers** (no wired transport —
see the maturity matrix). If you embed them yourself, the DoS guards that exist
are:

| Guard | Default | Source |
|---|---|---|
| Max concurrent streams | 128 → `REFUSED_STREAM` | `Http2Handler::MAX_CONCURRENT_STREAMS` |
| Max request body per stream | 8 MiB → `RST_STREAM` | `Http2Handler::MAX_REQUEST_BODY` |
| Max header block (HEADERS+CONTINUATION) | 64 KiB → `GOAWAY` | `Http2Handler::MAX_HEADER_BLOCK` |
| RST_STREAM rate limit (Rapid Reset) | 200 → `GOAWAY` | `Http2Handler::MAX_RST_STREAMS` |
| gRPC message size | 64 MiB → reject | `kMaxGrpcMessage` (grpc_handler) |

> HTTP/2 flow control, dynamic HPACK, and SETTINGS negotiation are **not
> implemented**. Do not expose `Http2Handler` to arbitrary public clients.

---

## Pipelines, channels & messaging

| Concern | Behavior | Source |
|---|---|---|
| `AsyncChannel` / `SpscChannel` | **bounded** ring; `send` applies back-pressure (`co_await`), `try_send` drops when full | channel headers |
| `MessageBus` publish | fan-out with back-pressure; O(1) RCU subscriber snapshot, no per-message alloc (v1.7.1) | `message_bus.hpp` |
| `RetryAction` / `CircuitBreaker` / `DeadLetterQueue` | resilience triad; breaker Closed-path is lock-free (v1.2.0) | `pipeline/resilience/` |
| `InMemoryIdempotencyStore` | **capacity-bounded** (default ~1M keys) with eviction — distinct-key OOM defense | `idempotency.hpp` |
| Worker teardown | `DynamicPipeline`/`PipelineGraph`/action wrappers drain in-flight items on destruction (no use-after-free) | tested in `action_lifetime_test` |

**Rule:** every channel is bounded. Choose `send` (apply back-pressure) vs
`try_send` (shed load) deliberately per stage.

---

## Crypto

- **PBKDF2** iterations are clamped to `kMaxPbkdf2Iterations` (100 M) as a DoS
  backstop, and PBKDF2/`password_hash` are CPU-bound — run them on an
  `OffloadPool` thread, never inline on a reactor.
- AEAD: prefer `seal_easy`/`open_easy` (random nonce) over raw `seal` to avoid
  nonce reuse. JWT: `verify_jwt_hs256` rejects `alg=none` and compares the
  signature in constant time.

---

## Reactor / runtime

- **Thread-per-core, shared-nothing.** A reactor and everything it owns
  (`Arena`, `TimerWheel`, `Task` frames) live on one thread and are not
  thread-safe. Touch another reactor only via `Reactor::post()`.
- `App` uses **`SO_REUSEPORT`** (one listen socket per reactor) — no accept-storm.
- Set `SIGPIPE` is ignored by `listen()`; writes to peer-closed sockets fail with
  `EPIPE` rather than killing the process.

---

## Pre-deploy checklist

- [ ] **TLS** terminated at a reverse proxy (no TLS in core).
- [ ] `app.set_max_connections(...)` set to your capacity (default is unlimited).
- [ ] Reverse proxy enforces the body/header/timeout limits you actually want
      (core defaults are 1 MiB body / 8 KiB headers / 30 s idle / 10 s read).
- [ ] `WsServerConfig` tuned for your workload (message size, send-queue cap,
      heartbeat) if using a standalone `WsServer`.
- [ ] `SIGTERM`/`SIGINT` → `app.stop()` graceful drain wired (App does this).
- [ ] PBKDF2 / `password_hash` run via `OffloadPool`, not on a reactor thread.
- [ ] Built `Release` (`-O3`); optionally `-DQBUEM_ENABLE_NATIVE_CRYPTO=ON` for
      hardware SHA-2/AES on hosts that support it.
- [ ] Health (`/health`) and metrics (`/metrics`) endpoints registered if you use
      them for orchestration.
