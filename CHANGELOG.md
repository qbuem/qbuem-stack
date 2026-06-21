# Changelog

All notable changes to **qbuem-stack** are documented here. Only released,
git-tagged versions are listed. The format follows
[Keep a Changelog](https://keepachangelog.com/), and the project adheres to
[Semantic Versioning](https://semver.org/) (`MAJOR.MINOR.PATCH`).

The 1.0.0 release is the first public stable baseline; everything before it was
pre-release internal iteration and is not tracked here.

---

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
