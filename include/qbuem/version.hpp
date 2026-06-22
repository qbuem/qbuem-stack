#pragma once

/**
 * @file qbuem/version.hpp
 * @brief Version constants for the qbuem-stack library.
 * @ingroup qbuem_version
 *
 * This header provides both compile-time version constants and preprocessor
 * macros:
 * - Use `qbuem::Version::major` (and friends) in templates and constexpr code.
 * - Use `QBUEM_VERSION_MAJOR` (and friends) in `#if` preprocessor conditions.
 *
 * ### Version history
 * Only released, tagged versions are listed. The full per-release notes live in
 * `CHANGELOG.md`; this is the short form.
 *
 * - 1.0.0: First public stable release. Async core (coroutine `Task<T>`,
 *           epoll/io_uring/kqueue reactor, dispatcher), HTTP/1.1 `App` server,
 *           pipelines, SHM IPC, crypto, zero-alloc memory. Hardened in the
 *           2026-06 audit: full TSan/ASan/UBSan CI; pipeline/action worker
 *           lifetime UAF fixes; SHM attacker-trust validation; misuse-resistant
 *           AEAD; opt-in QBUEM_ENABLE_NATIVE_CRYPTO (hardware SHA-2/AES).
 * - 1.1.0: High-level WebSocket server (`WsServer`/`WsConnection`) — non-blocking
 *           reactor-driven I/O, connection registry + rooms + broadcast,
 *           per-connection context, bounded back-pressure, heartbeat, strict
 *           RFC 6455 framing + UTF-8 validation, slowloris timeout. Plus a
 *           high-concurrency hardening pass (gRPC length-overflow guard; HTTP/2
 *           Rapid-Reset + DATA/CONTINUATION/HPACK DoS limits; RUDP reorder bound;
 *           PBKDF2 iteration cap; SHMBus topic-name UAF; Arena overflow guard;
 *           TimerWheel next_expiry O(1)). See docs/roadmap.md.
 * - 1.2.0: Misuse-resistant crypto (`crypto/secretbox.hpp` random-nonce AEAD +
 *           password_hash/verify; `crypto/jwt.hpp` HS256 sign/verify w/ strict
 *           alg check; `middleware/jwt_verifier.hpp`); `io/buffered_reader.hpp`;
 *           bounded DNS resolver; CircuitBreaker lock-free Closed path; App write
 *           paths fixed to not truncate on EAGAIN; `TcpStream::write_all/read_exact`.
 * - 1.3.0: `GenerationPool` — emplace / destroy / for_each_live (ABA-safe
 *           generation-tagged object pool for stable-handle entity storage).
 * - 1.4.0: `SpatialGrid<T>` — uniform-grid object spatial index for AOI /
 *           broad-phase neighbor queries (insert/move/remove, query-in-radius).
 * - 1.4.1: HTTP SSE / chunked flush-on-suspend — true server-push streaming
 *           (a handler can flush bytes mid-coroutine before it returns).
 * - 1.5.0: Precise fixed-timestep ticking. `TickLoop` (core/tick_loop.hpp):
 *           drift-compensated absolute-deadline scheduler, deterministic
 *           catch-up of missed ticks, zero-allocation jitter + work histograms,
 *           advance(now) (reactor) + run_pinned() (sub-ms) drive modes.
 *           `TickScheduler` (core/tick_scheduler.hpp): multi-rate systems,
 *           deterministic (seed,tick) RNG, pause/time-scale/step + render alpha,
 *           per-system metrics + overrun watchdog. Verified TSan/ASan/UBSan.
 * - 1.6.0: Documentation truth pass — docs rewritten to match the actual
 *           implementation; a feature-maturity matrix added; CHANGELOG added;
 *           fictional/aspirational docs removed. No API change.
 * - 1.7.0: `App::ws(path, handlers)` — high-level WebSocket endpoints served on
 *           the same App/port as HTTP routes, no hand-wired upgrade callback.
 *           Each connection is driven by a per-reactor `WsServer` (shared-nothing;
 *           callbacks race-free; rooms/broadcast are per-reactor). Verified
 *           end-to-end (HTTP + WS same port) under ASan/UBSan.
 * - 1.7.1: `MessageBus` publish fast path — subscribers held as an immutable
 *           RCU snapshot (`shared_ptr<const vector>`); publish/try_publish take
 *           an O(1) snapshot (one refcount bump) instead of copying every
 *           handler into two freshly-allocated vectors per message; writers
 *           copy-on-write. No API change; verified race-free under TSan.
 * - 1.8.0: Proof — reproducible benchmark results committed (bench/RESULTS.md,
 *           Apple M1 Pro baseline) + a CI bench-smoke job (build & run every
 *           benchmark); production checklist (docs/production-checklist.md);
 *           qbuem-game documented as the flagship real-world consumer. No API change.
 * - 1.8.1: CI publishes measured benchmark numbers per architecture (x86_64 +
 *           aarch64) in each run's Job Summary + as artifacts (committed server
 *           baselines). Final production-readiness verification sweep. No API change.
 * - 1.8.2: Correctness + CI hardening. Fixes a use-after-free in the `App::ws()`
 *           upgrade handoff on kqueue/epoll (unregister_event destroyed the
 *           read-callback closure, freeing the buffer backing the request's
 *           header views before the WsServer read them — a spurious 400 on
 *           macOS); buffer erase is now deferred until after the handoff. Also
 *           greens CI: clang-tidy (C-arrays → std::array), the fuzz link, and the
 *           benchmark job timeout. No API change; verified under ASan/UBSan/TSan.
 * - 1.9.0: SaaS-readiness + v1 finalization. Adds (all zero-dependency, behind an
 *           edge TLS terminator): RS256/JWKS bearer auth (`Rs256JwtVerifier`,
 *           `parse_jwks`, RFC-8725 iss/aud) + native `rsa_pkcs1_v15_sha256_verify`;
 *           per-tenant `quota` middleware; OTLP/JSON span export (`OtlpHttpExporter`);
 *           HTTP/2 SETTINGS negotiation + flow control. The umbrella now exposes the
 *           middleware/crypto/tracing SaaS surface; `examples/12-saas/`. Consolidation
 *           guide (docs/consolidation.md) marks redundant pipeline APIs for v2 removal
 *           (`SloObserver` deprecated). This wraps up the v1 line — subsequent changes
 *           are hotfix-only. Backwards-compatible (additive).
 */

/**
 * @defgroup qbuem_version Version
 * @brief Library version identification symbols.
 *
 * Both `constexpr` struct constants and C preprocessor macros are provided
 * so that the version can be queried at compile time and at the preprocessor
 * level. Follows Semantic Versioning 2.0.0 (https://semver.org).
 * @{
 */

#include <string_view>

namespace qbuem {

/**
 * @brief Compile-time version information for qbuem-stack.
 *
 * All members are `constexpr` and may be used in `static_assert` expressions
 * or as template non-type parameters.
 *
 * @note Semantic versioning rules:
 *   - `major`: incremented on backwards-incompatible API changes.
 *   - `minor`: incremented when new features are added in a backwards-compatible manner.
 *   - `patch`: incremented for backwards-compatible bug fixes only.
 *
 * @code
 * static_assert(qbuem::Version::major >= 1, "qbuem-stack 1.x required");
 * std::print("{}\n", qbuem::Version::string); // "1.9.0"
 * @endcode
 */
struct Version {
  /** @brief Major version number. Incremented on backwards-incompatible API changes. */
  static constexpr int major = 1;

  /** @brief Minor version number. Incremented when new backwards-compatible features are added. */
  static constexpr int minor = 9;

  /** @brief Patch version number. Incremented for backwards-compatible bug fixes only. */
  static constexpr int patch = 0;

  /** @brief Version string in "major.minor.patch" format (null-terminated). */
  static constexpr std::string_view string = "1.9.0";
};

} // namespace qbuem

/** @brief Major version number (for use in preprocessor `#if` conditions). */
#define QBUEM_VERSION_MAJOR 1

/** @brief Minor version number (for use in preprocessor `#if` conditions). */
#define QBUEM_VERSION_MINOR 9

/** @brief Patch version number (for use in preprocessor `#if` conditions). */
#define QBUEM_VERSION_PATCH 0

/** @brief Version string literal "major.minor.patch" (for use in preprocessor conditions). */
#define QBUEM_VERSION_STRING "1.9.0"

/** @} */ // end of qbuem_version
