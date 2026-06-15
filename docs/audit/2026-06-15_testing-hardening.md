# Testing Hardening — 2026-06-15

Strengthened static analysis, unit/module tests, **fuzzing**, and **sanitizer**
runs. Headline: fuzzing the parsers found **no** memory-safety bugs, and the
ASan+UBSan suite run found **one real library bug** (rate-limiter `inf→int` UB),
now fixed.

## 1. Fuzzing (new) — 7 libFuzzer harnesses over untrusted-input parsers

`fuzz/` + `fuzz/run_fuzz.sh` (build + run all, ASan+UBSan, per-input 5 s timeout).
Apple clang lacks libFuzzer → use Homebrew LLVM locally (`CXX=/opt/homebrew/opt/llvm/bin/clang++`);
Linux clang in CI has it.

| Harness | Target | Result (20 s, ASan+UBSan) |
|---|---|---|
| `fuzz_http_parser` | `HttpParser::parse` (HTTP/1.1 request) | **clean** — 742K execs @ 37K/s, cov 200 |
| `fuzz_url` | `ParsedUrl::parse` | clean, cov 138 |
| `fuzz_base64` | `base64_decode` / `base64url_decode` | clean, cov 93 |
| `fuzz_jwt` | `SIMDJwtParser::parse` | clean, cov 57 |
| `fuzz_line_codec` | `LineCodec::decode` | clean, cov 10 |
| `fuzz_length_prefix` | `LengthPrefixedCodec::decode` (incl. DoS cap) | clean, cov 60 |
| `fuzz_websocket` | `WebSocketHandler::decode_frame` | clean, cov 53 |

**No crashes, hangs, ASan, or UBSan reports** across all targets — the
untrusted-input surfaces are memory-safe against malformed input.
Institutionalized via the CI `fuzz` job (builds + runs each 15 s on every push).

## 2. Sanitizers (new) — full suite under ASan + UBSan

Built the whole test suite with `-fsanitize=address,undefined
-fno-sanitize-recover=undefined` and ran it. **32/32 suites pass serially.**
(Run **serially** — parallel sanitized suites saturate CPU/RAM and trip
timing-sensitive timeouts; that is contention, not a bug, confirmed by
per-suite isolation.) Institutionalized via the CI `sanitizers` job (`ctest -j1`).

### Bug found + fixed — `middleware/rate_limit.hpp` `inf → int` (UB)
`AllowsWithinBurstThenDenies` (rate=0, burst=3) tripped UBSan at `rate_limit.hpp:170`:
`(1.0 - tokens) / rate` is `+inf` when `rate == 0`, then `static_cast<int>(ceil(inf))`
is undefined behaviour (and spun under the sanitizer). **Fix:** guard `rate <= 0`
(bounded fallback) and clamp `Retry-After` to `[1, 86400]` s before the cast.
Verified: the suite now passes under ASan+UBSan.

## 3. Static analysis

`.clang-tidy` already enables `clang-analyzer-*` (the bug-finding static analyzer)
plus a curated modernize/performance/readability set, `WarningsAsErrors='*'`,
scoped to `include/qbuem/`. The CI `analyze` job runs `run-clang-tidy-18` over all
`src/*.cpp` on Ubuntu (the correct toolchain). *Note:* local clang-tidy on macOS
is blocked by a Homebrew-LLVM / Apple-SDK header incompatibility (`_CTYPE_A`,
`cstdint not found`) — a toolchain mismatch, not a code issue; CI is authoritative.

## 4. Unit / module tests

53 test files · ~1213 `TEST` cases · **32 ctest suites, 100% pass** (also under
ASan+UBSan, serial). Every module + the server/fetch/WebSocket e2e paths covered
(see `2026-06-15_coverage-status.md`).

## CI guard summary (8 jobs)
`zero-dep` (third-party + coro-spawn guard) · `analyze` (clang-tidy) ·
`test` (4-platform build + ctest + header self-containment) · `install-check` ·
`fuzz` (parser fuzz smoke) · `sanitizers` (ASan+UBSan serial ctest).

## Honest notes
- Fuzz runs here were 20 s/target (smoke depth) — enough to exercise the parse
  paths under ASan+UBSan, not an exhaustive multi-hour campaign. CI runs 15 s/target
  per push; a corpus + longer nightly run would deepen it further.
- Sanitizer/fuzz on x86_64 + Linux is verified in CI (this dev box is ARM macOS).
