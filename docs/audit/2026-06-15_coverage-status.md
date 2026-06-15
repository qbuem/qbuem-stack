# Module Coverage Status — 2026-06-15

Goal (owner): **every feature works; good integration no matter which modules are composed; ~100% coverage across all modules.**

## How coverage is measured here (and an honest caveat)

`llvm-cov` line-percentage is **not a trustworthy metric for this library**: it is
header-only and heavily templated, so the source-based coverage mapping only
materializes the small subset of code actually instantiated and not inline-elided
(an instrumented run reports ~228 mapped lines at 100% — which badly *understates*
the real surface rather than measuring it). Reporting that number would be
misleading. The instrumented build exists (`build_cov`, `-fprofile-instr-generate
-fcoverage-mapping`) for spot checks, but the metric we hold ourselves to is:

> **Functional coverage** — every module's public API is exercised by a
> deterministic, ASan-clean, passing unit test and/or a runnable example.

## Totals

| Metric | Value |
|---|---|
| Test files | 53 |
| `TEST()` cases | 1206 |
| ctest suites | 31 (100% pass) |
| Examples that run (excl. servers) | 58 / 58 |
| Build | 0 warnings / 0 errors |
| Per-module test authoring verified | ASan-clean, all pass |

## Per-module test coverage

| Module | Test files | Notes |
|---|---:|---|
| core | 16 | task, dispatcher, reactor (epoll/io_uring/kqueue), arena, awaiters, timer_wheel, async_logger, cpu_hints, micro_ticker, session_store, transport, numa/huge_pages (graceful) |
| pipeline | 13 | static/dynamic/graph (split+merge), hot_swap, channels, batch/window/stateful, message_bus, priority/spsc/async channel, resilience, saga/canary/checkpoint/idempotency/slo, topology |
| buf | 5 | generation_pool, lock_free_hash_map, intrusive_list, kqueue_buffer_pool, inplace_function, grid/tiled bitset, simd_erasure |
| http | 5 | parser, request/response, router, url, middleware glue |
| io | 5 | read_buf/write_buf, buffer_pool, iovec/scattered_span, io_slice, direct_file, zero_copy, socket_opts |
| middleware | 5 | cors, rate_limit, content_type, sse, static_files, body_encoder, token_auth, security headers, request_id |
| security | 4 | simd_jwt, jwt_action, jwt verify/tamper |
| crypto | 3 | sha256/512 family, hmac, hkdf, pbkdf2, chacha20, poly1305, aes-gcm (NIST KAT), base64/url, secure_zero, random |
| db | 3 | value (variant), driver (mock), connection_pool, smart_cache (seqlock) |
| net | 3 | socket_addr, socket_compat, loopback, uds/udp construction |
| shm | 3 | shm_channel, shm_bus, layout |
| tracing | 3 | trace_context (W3C), span, exporter, sampler, lifecycle_tracer, trace_logger |
| codec | 2 | frame, line, length_prefix (DoS cap), http1 |
| server | 2 | http1/http2/ws/grpc handlers (+ exercised by running examples) |
| config | 1 | config_manager, Secret<T> |
| transport | 1 | plain_transport |
| compat | (transitive) | `std::print` polyfill — exercised by all 53 files via `println` |

## This phase (2026-06-15) — +475 tests across 10 new suites

Authored + verified (each ASan-clean, all pass) via a 10-agent workflow:

| Suite | Tests | Highlights |
|---|---:|---|
| config_tracing | 95 | Secret<T> redaction/wipe, ConfigManager coercion + error paths, W3C traceparent roundtrip + invalid inputs, span RAII export, Prometheus exporter, lifecycle ring |
| pipeline_actions | 67 | batch/window/stateful, priority/spsc/async channels, message_bus, service_registry, dynamic_router, backpressure |
| crypto_depth | 54 | HKDF/PBKDF2/SHA-384/512-256 KATs, ChaCha20 + Poly1305 RFC 8439 vectors, base64url edge, secure_zero |
| core | 46 | Task value/void/detach/error, awaiters, cpu_hints, PlainTransport, micro_ticker, numa/huge_pages graceful |
| middleware | 45 | cors preflight, rate-limit bucket, content_type, sse, static_files (+ path-traversal reject), token_auth |
| net | 42 | socket_compat over socketpair, UDS fd passing, loopback UDP, construction/bind |
| buf | 37 | intrusive_list (full), kqueue_buffer_pool, generation_pool stale-handle, lock_free_hash_map error paths |
| db | 37 | value variants, driver mock, connection_pool, smart_cache seqlock |
| codec | 28 | incremental/partial decode, length-prefix DoS cap boundary, http1 smuggling/413 rejects |
| io | 24 | read/write buf, iovec, io_slice, direct_file (/tmp), zero_copy sendfile, socket_opts graceful |

All error paths (`std::expected` value AND `.error()`) are exercised; file-based
tests use `/tmp` and clean up; platform-specific paths (O_DIRECT, sendfile, numa)
tolerate `errc::not_supported` rather than crash.

## Final phase (2026-06-15, round 3) — close the last gaps

### Server + fetch + WebSocket e2e (`tests/server_e2e_test.cpp`, 7 tests, 13 runs non-flaky)
- **WebSocket** byte-level: RFC 6455 handshake-key vector, frame encode/decode
  roundtrip (Text/Binary, masked+unmasked, 1/2/8-byte length encodings + boundaries),
  Ping/Pong/Close, truncated-buffer contract.
- **Http1Handler** over a `socketpair`: real `GET /hi` through `on_connect`+`on_frame`,
  asserts the routed `200`/`pong` response bytes.
- **fetch() end-to-end** over loopback: raw HTTP server coroutine + `co_await
  fetch("http://127.0.0.1:port/").send(st)`, asserts `status()==200` + `body()=="pong"`.

### Broken/uncompilable headers fixed (found by an orphan + self-contained sweep)
These were **orphaned** (included by nothing) so their compile errors were never
caught — each is a real "feature that didn't build":
- `server/http1_handler.hpp` — used `http::Request`/`http::Router` but those types
  live in `namespace qbuem` (no `qbuem::http` alias) → wouldn't compile for any
  consumer. Fixed the namespace; now compiles + is exercised by server_e2e.
- `http/backoff.hpp` — missing `#include <thread>` (used `std::this_thread`).
- `http/trace_middleware.hpp` — `co_await next()` (Task<void>) assigned to bool, and
  a 3-arg `end_span` call (the API returns an RAII Span). Fixed both.
- `io/direct_file.hpp` — `O_DIRECT` is Linux-only; unguarded → broke macOS. Now
  guards O_DIRECT and applies `fcntl(F_NOCACHE)` on macOS.

### Header self-containment (composability)
Swept **all 145 headers** for standalone compilation ("include this header alone").
Fixed 3 that relied on transitive includes (so any consumer composing just that
module would have broken): `buf/simd_erasure` (`<optional>`), `io/socket_opts`
(`<sys/socket.h>`), `pipeline/windowed_action` (`<qbuem/core/dispatcher.hpp>`).
**Result: 145/145 headers compile standalone** — any module can be included on its own.

### Cross-platform verification (x86_64 / arm64 × linux / mac)
`.github/workflows/ci.yml` runs a 4-platform matrix — ubuntu-24.04 (x86_64),
ubuntu-24.04-arm, macos-15 (arm64), macos-13 (x86_64) — each: build all examples +
full `ctest` + zero-dependency guard + clang-tidy + install-layout check. All the new
tests run on every platform there (the only way the x86/Linux paths get verified, as
this dev box is ARM macOS).

## Final status (all rounds)
- **145/145 headers self-contained** · **58/58 examples run** · **ctest 32 suites /
  ~1213 tests, 100% pass** · build 0 warnings / 0 errors · per-module tests ASan-clean.
- Library bugs fixed across the verification: DynamicPipeline `hot_swap`,
  SHMSegment `create` (macOS leaked segment), `http1_handler`/`backoff`/
  `trace_middleware`/`direct_file` compile breaks, `subpipeline_action` deprecation,
  + ~13 example runtime bugs (IIFE coroutine-lambda UAF class, teardown, asserts).

## Remaining gaps (honest)
- Server handlers tested at the `on_frame`/byte level (http1) + via running examples
  (http2/ws/grpc); not a full multi-connection socket stress harness.
- HTTPS/TLS is intentionally out of scope (no third-party crypto dep) — `fetch` is HTTP-only.
- Exact line-% across all 145 templated headers is not a meaningful single number
  (see measurement caveat above); functional coverage is the metric held to.
