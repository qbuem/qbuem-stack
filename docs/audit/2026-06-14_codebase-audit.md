# qbuem-stack — Full Codebase Audit (2026-06-14)

> Comprehensive 전수조사 of the C++23 zero-copy/zero-latency/zero-allocation/zero-dependency SDK,
> commissioned as the foundation for a next project. Covers `include/`, `src/`, `tests/`, `examples/`, `docs/`.
> Method: 7 parallel module reviewers + a real `cmake` build on macOS (Apple clang 21) + orchestrator
> verification of the highest-severity claims. Each finding tagged with verification level.

**Verification legend:** `[V]` personally verified by orchestrator (build log / direct read) · `[H]` agent-traced, high confidence · `[M]` agent-traced, medium / usage-dependent.

---

## 0. Executive Summary

The library is **architecturally ambitious and partially excellent** (the crypto scalar core, SHA-NI/ARM-SHA2, `scattered_span`/`IOVec`, the type-safe `StaticPipeline`, kqueue/io_uring/PCIe-VFIO real implementations), but it is **not currently a dependable foundation** without remediation. Three structural truths:

1. **It does not build on macOS** — the documented first-class platform — and macOS is the current dev box. `[V]`
2. **Several advertised subsystems are non-functional** (stubs with no implementation, or docs describing capabilities the code doesn't have). Building on them silently fails or won't link. `[V]/[H]`
3. **The "zero latency / zero allocation" pillars are aspirational, not enforced** — the reactor callback path, the pipeline stage path, and the HTTP/WebSocket server write path all violate them, some via *blocking syscalls on the reactor thread*. `[H]`

There is one **catastrophic correctness bug** (AES-GCM silently encrypts nothing on ARM `[V]`) and a cluster of remote-input DoS vectors and coroutine-lifetime UB.

**Recommended posture:** treat the *verified working subset* as the foundation, fix the P0s + the macOS build, honestly demote/remove the fictional surface, and write the usage guide only for what is real. Detailed plan in §8.

---

## 1. Build Status — BROKEN on macOS `[V]`

`cmake -B build_review -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release` then `cmake --build` → **19 compile errors**; only 1 example + 2 test binaries linked. (The earlier "exit code 0" was the trailing `echo`, not the compiler.)

Root causes — Linux-only symbols used in headers with no macOS fallback, pulled transitively by the umbrella `qbuem_stack.hpp`, so even `hello_world` fails:

| Symbol(s) | Headers | Fix |
|---|---|---|
| `SOCK_NONBLOCK`, `SOCK_CLOEXEC` | `net/udp_socket.hpp`, `udp_multicast.hpp`, `udp_mmsg.hpp`, `unix_socket.hpp`, `tcp_listener.hpp` | macOS: create socket then `fcntl(F_SETFL,O_NONBLOCK)` + `FD_CLOEXEC` — the pattern already exists in `tcp_stream.hpp`/`uds_advanced.hpp` |
| `recvmmsg`, `sendmmsg`, `mmsghdr`, `MSG_WAITFORONE` | `net/udp_mmsg.hpp` | wrap whole file in `#ifdef __linux__`; provide a loop-based macOS fallback or `errc::not_supported` |
| `cpu_set_t`, `sched_getaffinity` | `tools/affinity_inspector.hpp`, `core/numa.hpp` | guard under `#ifdef __linux__`; macOS uses `thread_affinity_policy` or no-op |
| `std::launder` w/o `#include <new>` | `buf/generation_pool.hpp` | add `#include <new>` (libstdc++ leaks it; libc++ doesn't) |
| `MulticastSocket` returned by value from `Result<>` but non-movable | `net/udp_multicast.hpp:176,231` | add move ctor or return via factory that constructs in-place |
| `#include` **inside** `numa.hpp::open()` body | `core/numa.hpp:294-296` | move includes to file scope (also fixes the perf-counter `#if` guard always being false) `[V]` |

**One header sweep makes the SDK compile + 9 examples build on macOS.** This is the prerequisite for everything else (refactor verification, tests, the usage guide).

---

## 2. P0 — Correctness / Security / UB (fix before depending on these)

### Crypto
- **AES-GCM silent no-op on ARM** `[V]` — `aes_gcm.hpp:83-97,322-371`. On ARM, `has_aes_ni()` returns `true` but `seal()`/`open()` bodies are `#if QBUEM_AES_NI` (x86 only) → `seal()` writes nothing, `open()` always errors. **Confidentiality + integrity both lost with a false success.** Fix: make `has_aes_ni()` return `false` on ARM until a real PMULL path exists (so `create()` fails loudly), or implement ARM AES. Add a `static_assert`/error so a no-op `seal` can never succeed.
- **CRC32 computes a different polynomial per arch** `[H]` — `security/simd_validator.hpp:437-495`. Scalar = IEEE `0xEDB88320`; SSE4.2/ARM use the CRC32**C** (Castagnoli) instructions. Same data → different checksum across builds → cross-platform validation/interop failure. Fix: pick one (rename to `crc32c` + use HW everywhere, or implement CLMUL/PMULL IEEE-CRC on both).

### Coroutines / async core
- **`Task::await_resume()` UB** `[H]` — `core/task.hpp:173`. `std::move(*handle.promise().value)` with no `has_value()` guard; reachable when the configurable `unhandled_exception` handler lets control return without a value. Fix: keep `std::terminate` as the only unhandled-exception policy (matches the no-exceptions pillar), or store `expected` in the promise.
- **`Dispatcher::spawn(Task<Result<void>>)` cross-thread resume** `[M]` — `src/core/dispatcher.cpp` + `dispatcher.hpp:184`. Wrapper coroutine's `continuation` written on caller thread, read on reactor thread without a clear happens-before. Add a TSan test; document the synchronizing edge.
- **io_uring buffered-recv loses callbacks** `[H]` — `src/core/io_uring_reactor.cpp:567`. One `pending_cb` per buffer-group `bgid`; a second `recv_buffered` on the same bgid overwrites the first → stranded coroutine / wrong dispatch. Key the callback by op token.

### Pipeline
- **`virtual` function template** `[V]` — `pipeline/hot_swap.hpp:193-199,207`. Ill-formed C++; `HotSwapMixin` is uncompilable dead code. Delete the mixin (DynamicPipeline has its own hot_swap) or make it CRTP.
- **Duplicate `SubpipelineAction` (ODR)** `[H]` — `pipeline/subpipeline_action.hpp:53` vs `shm/message_bus.hpp:525`: two classes, same name+namespace, different members. Rename one.
- **`SubpipelineAction::operator()` shuffles results** `[H]` — `subpipeline_action.hpp:87-108`. Pushes one item, then `recv()`s *whatever comes out next* from a multi-worker inner pipeline → silent cross-request result mismatch. Needs correlation id or a serialized single-worker inner pipeline.
- **`DynamicPipeline::hot_swap` stops processing** `[H]` — `dynamic_pipeline.hpp:184-211`. Replaces the worker factory and requests stop on old workers but never respawns → stage becomes a black hole, pipeline wedges. The flagship "hot-swap" doesn't hot-swap a *running* pipeline.
- **`CircuitBreaker` never opens under mixed traffic** `[M]` — `circuit_breaker.hpp:117-133`. Any single success in `Closed` zeroes the failure counter → alternating fail/success never reaches threshold. Use a rolling window or document "consecutive only."

### Networking / web (server-side hot path is not actually async)
- **Blocking `::write` on the reactor thread in `Http1Handler`** `[H]` — `server/http1_handler.hpp:287-297` (+ writev path). On a non-blocking fd, `EAGAIN` → `n<=0` → response silently dropped under backpressure; on a blocking fd → event loop stalls. The async `TcpStream::write/writev` awaiters exist and aren't used. **L1 violation on the core request path.**
- **Blocking `::read`/`::write` in `WebSocketHandler::run`** `[H]` — `server/websocket_handler.hpp:210,684`. Same problem per frame; first `EAGAIN` is treated as EOF.
- **WebSocket 64-bit payload length → unbounded `resize()` (DoS)** `[H]` — `websocket_handler.hpp:523-555`. No max-frame cap. Add a configurable limit before `resize`.
- **`LengthPrefixedCodec` 4-byte prefix → up to 4 GiB `reserve()` (DoS)** `[H]` — `codec/length_prefix_codec.hpp:109-115`. Add `max_frame_size`.
- **`std::stoul`/`std::stoi` in HTTP/NUMA parsers throw → `std::terminate`** `[H]` — `src/http/parser.cpp:227`, `core/numa.hpp`. Exceptions are forbidden (`Task::unhandled_exception`→terminate). Use `std::from_chars`. A crafted `Content-Length` crashes the server.
- **HTTP/2 detached handler double-dispatch + discarded errors** `[M]` — `server/http2_handler.hpp:1009`. HEADERS+END then DATA+END can invoke the handler twice; `t.detach()` discards a throwing coroutine → terminate. Guard HALF_CLOSED_REMOTE; track via dispatcher.

### SHM / IPC
- **`SHMChannel<T>` MPMC torn read** `[V]` — `shm/shm_channel.hpp:292,550`. Documented MPMC, but every consumer `memcpy`s into the *same* `recv_buf_` member and returns `&recv_buf_`. Two consumers race → torn/overwritten value. Fix: return `std::optional<T>` by value (the slot is recycled right after the copy anyway, so the "zero-copy view" claim is already false).
- **`ReliableCast::try_consume` TOCTOU** `[H]` — `shm/reliable_cast.hpp:204-226`. Under default `DropSlow`, producer can overwrite the slot mid-`memcpy`; no per-slot seqlock/post-copy revalidation. Add a generation seqlock.
- **Stub modules would fail to link if used** `[V]` — no definitions anywhere, no CMake target: `rdma/rdma_channel.hpp`, `spdk/nvme_io.hpp`, `ebpf/ebpf_tracer.hpp`, `db/simd_parser.hpp` (refs a non-existent `db/simd_parser.cpp`), `shm/futex_sync.hpp` (async `wait/wake/lock/acquire`). They're pulled by the umbrella header. See §4.

---

## 3. P1 — Pillar Violations (the "zero-cost" claims are not enforced)

**Zero Allocation (A7/A8/A2) — `std::function` / `shared_ptr` / per-op `vector` on hot paths:**
- `std::function` is the universal callback/stage type across the **reactor ABI** (`reactor.hpp:98`, all awaiters), **pipeline stages** (`action.hpp:62` + every action), and **server handlers** (router, WS message, HTTP/2 request, gRPC stream) — heap closures + indirect calls per I/O / per message. `[H]` Highest-leverage fix: a small-buffer `inplace_function`/`function_ref`, or store `coroutine_handle` directly.
- `std::make_shared` per kqueue timer registration `[H]` (`kqueue_reactor.cpp:87`); `shared_ptr` per HTTP/2 stream `[H]`.
- `std::vector` constructed per `poll()` iteration in io_uring + the wake-drain of all three reactors `[H]` — promote to reused members (kqueue already does for `events_`).

**Zero Copy (C5/C7):**
- `ContextualItem<T>` **copied** into every fan-out branch (not moved even for the single/last successor); `std::any` constructed per item just to run predicates in `PipelineGraph`/`DynamicRouter` `[H]` (`pipeline_graph.hpp:540`). Drop `std::any` (the graph is already typed); move into the last successor.
- Per-request `std::string` + `std::transform` to case-fold `Upgrade`/`Connection` headers `[H]` (`http1_handler.hpp:142`); `Response` builds an `unordered_map<string,string>` + a fresh header string per response `[H]`. Mirror `Request`'s flat inline header store.

**Zero Latency (L1/L2/L6):**
- `Dispatcher::run` polls with a **100 ms** timeout `[H]` (`dispatcher.cpp:35`) — 100× the ≤1 ms rule; epoll/io_uring `stop()` don't wake the blocked poll `[H]`.
- Busy-spin "async" waits in windowed/batch/debounce/throttle/stream ops and every `drain()` (`co_await Yield{}` in a tight loop) `[H]` — burns a core; `RetryAction::sleep_async` shows the correct `register_timer` pattern.
- Blocking `pread`/`pwrite` in `AsyncFile` (no `co_await`, file opened without `O_NONBLOCK` despite the doc) `[H]`; blocking `recvmsg(MSG_WAITALL)` in `uds_advanced.hpp` `[H]`; blocking `sendfile` loop in `qbuem_stack.cpp` static file path `[M]`.

**Hardware (H1/H2) — SIMD parity is fraudulent in several places** `[H]`:
- ChaCha20 AVX2 (macro defined, **no `_mm256_*` code** → scalar on x86), base64 AVX2 encoder (stub returns 0), SHA-512 "ARMv8.2" (dead lambda, always scalar), SIMD JSON validator AVX2/SSE (compute masks then `(void)` them → scalar), JWT `is_base64url` ("pshufb" doc, scalar `ranges::all_of`). Either implement or delete the macro + correct the perf docs.
- `SHMHeader` head+tail share one cache line → producer/consumer false sharing `[H]` (`shm_channel.hpp:74`). `SmartCache::CacheSlot` stacks `alignas(64)` per member → ~256B slots, wrong padding math `[H]`.

**Other P1:** `request_id` UUIDs from `mt19937` (predictable) instead of the in-tree CSPRNG `[H]`; no secure key zeroization anywhere `[H]`; `Action` worker silently drops items on error (no DLQ/observer hook by default) `[H]`; chunked-body read in fetch client is O(n²) with a false-positive terminator `[H]`.

---

## 4. Stub / Doc-Fiction Inventory (matters most for a "foundation") `[V]/[H]`

| Module | Reality | Doc claims |
|---|---|---|
| `rdma/rdma_channel.hpp` | **interface only, no impl, no build target** | full RDMA verbs API |
| `spdk/nvme_io.hpp` | **interface only, no impl** | NVMe user-space I/O |
| `ebpf/ebpf_tracer.hpp` | **interface only, no impl** | eBPF tracing |
| `db/simd_parser.hpp` | **all parsers undefined**, refs non-existent `.cpp` | SIMD Postgres row parse |
| `shm/futex_sync.hpp` | **async wait/wake/lock/acquire undefined** | `IORING_OP_FUTEX` sync |
| `db/smart_cache.hpp` | real **in-process** seqlock cache; `name_` dead, no `shm_open` | "shared memory across all processes" + "RDMA WRITE invalidation" |
| `db/connection_pool.hpp` | real mutex+vector; ~120 lines dead lock-free scaffolding | "O(1) lock-free Vyukov MPMC ring" |
| `shm/shm_channel.hpp` futex path | 1 ms `AsyncSleep` poll fallback, `wake` is no-op | "`IORING_OP_FUTEX_WAIT` ~150 ns p99" |
| `pipeline/hot_swap.hpp` | uncompilable (`virtual` template) dead code | Seal→Drain→Swap→Resume |
| `pipeline/observer.hpp` | dead duplicate of `observability.hpp` (ODR trap) | — |
| `examples/10-hardware/hardware_io` | `std::println`s API names as **strings**, calls nothing | "demonstrates hardware I/O" |

**Genuinely real & solid:** `xdp/` (cleanly `#ifdef`-gated libbpf), `pcie/` (real VFIO impl + non-Linux stub; bugs: IOMMU unmap leak `[H]`), crypto scalar core + SHA-NI/ARM-SHA2 + ChaCha20/Poly1305 (correct, constant-time, tag-before-plaintext) `[H]`, `scattered_span`/`IOVec` (lifetime contract correct, well-documented) `[H]`, `StaticPipeline`+`AsyncChannel`+`RetryAction`/`DLQ` (the trustworthy pipeline subset) `[H]`, `TimerWheel`/`Arena`/`FixedPoolResource` (modulo `next_expiry_ms` O(n) `[H]` and arena block-reuse `[H]`).

---

## 5. Test Coverage Matrix `[H]`

Framework: GTest (34/36 files consistent; `neon_validation` + `http_test` roll their own `main`).

| Tested behaviorally | Partial | **Zero behavioral tests** |
|---|---|---|
| pipeline (5/6 dims), core, io, buf, middleware, crypto (x86 paths skipped on arm64), shm, tracing, codec | http (Request/Response/parser only), security (JWT structural+expiry; **signature verify untested**), server (near none), db (Value only) | **net (12 hdrs — address-parse only), protocol (5), reactor event loop (test is mislabeled), tools, transport, xdp, grpc, config; db DAO/ORM/driver** |

- Pipeline missing dimension: **scale-IN** (runtime worker reduction). `[H]`
- `reactor_test.cpp` tests Task/Arena/Result, never instantiates a reactor. `[H]`
- Tautological/`SUCCEED()`-only tests: `SloConfig.CanBeConstructed`, `scatter_gather.ConstructionDoesNotThrow`, `HasRdrandReturnsBool`, etc. `[H]`
- `pcie_layout_test`/`spdk_layout_test` are ABI/layout checks, not coverage (pass even if every method body were `terminate()`). `[H]`
- The security-critical question — *does a tampered JWT get rejected?* — is **unverified**. `[H]`

---

## 6. Examples Assessment `[H]`

- **9 examples fail to compile on macOS** (the net-header break) — incl. `hello_world`, `tcp_echo_server`, `game_server`, `trading_platform`, `middleware`. `[V]`
- **9 examples orphaned** (on disk, never in CMake → silent rot): `fetch_stream`, `http2_client`, `udp_advanced`, `backpressure_monitor`, `dynamic_router`, `stateful_window`, `inspector_dashboard`, `lifecycle_tracing`, `hardware_chaos`. `[H]`
- Example **count is wrong 3 ways**: README prose "58", table 69 rows, disk 60 dirs, CMake builds 51. CLAUDE.md says both "44" and "58". `[H]`
- No `cout`/`printf`/`throw`/raw-`new` anti-patterns in the teaching code (one `fprintf` in a CHECK macro). `[H]`
- **Canonical references for the usage guide** (verified to call the API, not string-print): pipeline → `sensor_fusion`; dynamic → `dynamic_hotswap`; graph → `fanout`; stream ops → `stream_ops`; SHM/IPC → `shm_channel` + `ipc_pipeline`; scatter-gather → `scatter_send`; HTTP server → `hello_world`; fetch client → `http_fetch`; reactor → `async_timer`; crypto → `crypto_primitives`; resilience → `resilience`; tracing → `tracing`; websocket → `websocket`; arena → `arena`.

---

## 7. Contract / Doc Inconsistencies

- **M1 vs reality:** `common.hpp:112` defines the `Result<T>`/`unexpected` aliases that Pillar 6 **M1 explicitly forbids**, and the whole codebase uses them. **Recommendation: keep the alias (idiomatic, pervasive) and amend M1** rather than churn 200 files. `[V]`
- "Draco WAS" naming in `qbuem_stack.hpp` `App` doc; "C++20" in `task.hpp` `@brief` (library is C++23). `[H]`
- `AsyncLogger` doc "lock-free SPSC" vs spinlock-guarded MPSC impl; `BufferPool`/`GenerationPool` docs overstate MT/ABA safety. `[H]`
- CLAUDE.md "Key Files" points to `transport/` for codecs that live in `codec/`. `[H]`

---

## 8. Recommended Remediation Plan (phased)

**Phase 0 — Make it real (prerequisite).** Fix the macOS build (§1 header sweep) → clean `cmake` + `ctest` baseline on the dev box. ~1 focused pass, mechanical, high confidence.

**Phase 1 — Stop the bleeding (P0 correctness/security).** AES-GCM fail-loud-on-ARM; `from_chars` in parsers (kill the terminate-on-bad-input); DoS caps (WS/length-prefix); `SHMChannel` return-by-value; delete uncompilable/dead code (`hot_swap.hpp` `virtual` template, `observer.hpp` dup); rename duplicate `SubpipelineAction`. Each fix → recompile + targeted test.

**Phase 2 — Honest surface.** Decide per §4 module: delete vs. mark-experimental-and-exclude-from-umbrella vs. implement. Correct all SIMD/perf docs to match reality. Remove the fictional doc claims.

**Phase 3 — Pillar enforcement (the actual "refactor").** The high-leverage one: replace `std::function` in the reactor ABI + `Action` with a non-allocating callable; reused poll buffers; timer-driven (not busy-spin) windows/drains; move-not-copy in fan-out; async (not blocking) server write path. Biggest effort; do in ranked waves.

**Phase 4 — Tests + Usage Guide.** Add the missing-but-critical tests (net loopback round-trip, JWT tamper-reject, reactor loop, pipeline scale-in). Write the usage guide **scoped to the verified-working subset** (§6 canonical examples), with an explicit "experimental / not-yet-implemented" appendix so the guide never teaches a fiction.

---

---

## 9. Remediation Progress (2026-06-14)

Scope decision (user): both macOS+Linux targets, implement priority stubs, full
refactor. Worked autonomously; everything below was built (`build_review`, Apple
clang 21) and tested on macOS — **0 errors / 0 warnings, ctest 20/20**.

### ✅ Done & verified on macOS
- **Phase 0 — build portability.** New `net/socket_compat.hpp` (`make_socket`/
  `accept_nonblock_cloexec`) routed through all 8 socket sites; `udp_mmsg`
  Linux/macOS split (recvmsg/sendmsg fallback); `cpu_hints.hpp` `#include`-in-
  namespace bug; `generation_pool` `<new>`; `affinity_inspector`/`numa` guards;
  `micro_ticker` example → platform reactor; `pcie_layout_test` overload fix.
  Result: 1→51 examples, 2→20 tests build.
- **AES-GCM ARM (P0).** Implemented ARM AES (`vaeseq`/`vaesmcq`) + software key
  schedule. Added a **NIST GCM known-answer test** which exposed that the GHASH
  was non-spec on **x86 too** (round-trip-only tests masked it). Replaced with a
  portable spec-correct `gf_mul128` shared by both backends. KAT passes.
- **Phase 1 P0s.** `parser.cpp` Content-Length `from_chars` (was `std::stoul` →
  terminate on malformed input); WebSocket 16 MiB + LengthPrefixCodec 64 MiB DoS
  caps; deleted dead `hot_swap.hpp` (ill-formed `virtual` template) + dead
  `observer.hpp` (ODR trap); `Task::await_resume` UB guard; CircuitBreaker
  consecutive-failure semantics documented; fixed 6 pre-existing warnings
  (incl. a UB `-Wreturn-type` in `ipc_pipeline` example).
- **Phase 2 security.** CRC32 → consistent **CRC32C** across x86/ARM/scalar
  (was IEEE on scalar, Castagnoli on HW — interop bug); `request_id` UUIDs now
  from the CSPRNG (was predictable `mt19937`).
- **CI.** Added a **macOS (Apple Silicon, kqueue) job** + enabled example builds
  in CI — exactly the gap that let the macOS break ship undetected.
- **Usage guide** — `docs/usage-guide.md`, scoped to the verified-working subset
  with an explicit experimental/not-implemented appendix.
- **Test coverage +103 cases** (multi-agent workflow, each self-verified): net
  loopback (TCP/UDP/UDS round-trip), JWT sign/verify + tamper-reject (security
  gap closed), router (17), buf pools (GenerationPool/LockFreeHashMap/erasure),
  io buffers, content-type/body-encoder middleware, real kqueue event-loop →
  `qbuem_coverage_tests` + `qbuem_reactor_loop_tests`. **ctest now 22 suites / 100%.**
- **Sample coverage +6 examples** (workflow): config, transport-codec, dev-tools
  (new modules) + 3 recovered orphans; fixed 3 library header-include bugs
  (`<unistd.h>`/`<thread>`/`<charconv>`). 6 broken orphans deferred (documented
  inline in examples/CMakeLists.txt). **57 examples build.**
- **Performance — `inplace_function`** (`buf/inplace_function.hpp`): a zero-
  allocation `std::function` alternative, tested (8 cases) + benchmarked
  (`bench/bench_callback`): **8.7× faster, 0 heap allocs/op** vs std::function on
  a 32-byte-capture escaping hot-path callback (std::function = 20.4 ns + 1 malloc;
  inplace_function = 2.3 ns + 0). The foundation for de-allocating the reactor/
  timer internal callback path (small known captures).
- **Performance — SHM false sharing fixed** (`shm/shm_channel.hpp`): producer
  `tail`, consumer `head`, and read-mostly metadata now sit on **3 separate
  cache lines** (was: tail+head in one 64B line → MESI ping-pong on every
  commit/consume). The IPC/SHM hot path (H4 pillar).
- **Performance/correctness — Arena block reuse** (`core/arena.hpp`): `reset()`
  now reuses already-allocated trailing blocks instead of pushing new ones every
  overflow — fixes unbounded `blocks_` growth across reset cycles (audit P2-11),
  the per-request hot path. Regression test added (`block_count()` stays bounded).
- Also fixed 15 pre-existing `[[nodiscard]]`-ignore warnings in tests (kept the
  0-warning build).

### ⏳ Remaining (rationale)
- **AVX2 SIMD parity** (ChaCha20 / base64 / JSON validator) + **SHA-512 ARM**:
  cannot be compiled/verified on an ARM Mac — must be done with the now-enabled
  x86 CI to avoid shipping untested SIMD (the exact failure mode behind the GHASH
  bug). Functionally correct today via scalar/NEON fallback.
- **Key zeroization** (`secure_zero` in crypto dtors): defense-in-depth, pending.
- **Phase 3 pillar refactor** (`std::function` reactor/stage ABI →
  non-allocating callable; async server write path; move-not-copy fan-out;
  false-sharing fixes): large, cross-cutting, and needs benchmarking to justify
  — warrants a dedicated, measured effort, not a rushed pass.
- **Stub modules** (rdma/spdk/ebpf/futex/simd_parser): need real hardware/Linux;
  recommend gating behind `QBUEM_HAS_*` + umbrella exclusion until implemented.
- **More tests**: net loopback round-trip, JWT tamper-reject, reactor event loop,
  pipeline scale-in. **SubpipelineAction ODR** (latent, never co-included).

*Original assessment from a 7-reviewer parallel audit + macOS build. Findings
tagged `[V]`/`[H]`/`[M]` per honest-reporting protocol.*
