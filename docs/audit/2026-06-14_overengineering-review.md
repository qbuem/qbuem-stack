# qbuem-stack Over-Engineering Review — 2026-06-14

**Method:** 9 parallel reviewers (Opus 4.8) audited all **192 headers**, classified each
against the owner mandate, with usage cross-checked against `examples/` and `tests/`.

**Owner mandate (calibration):**
- Supported targets ONLY: **Linux x86_64 · ARM64 boards (Jetson-class) · Mac aarch64**.
- **No exotic-hardware** support (RDMA NIC, NVMe passthrough, BPF kernel, AF_XDP NIC, PCIe/VFIO).
- **Zero-dependency**: stdlib + OS/arch intrinsics only. Anything needing a third-party lib
  (libibverbs, libbpf, SPDK, quiche, OpenSSL) to function = remove.
- Keep only essentials; aggressively cut over-engineering.

**Aggregate verdict (192 headers):**

| Recommendation | Count |
|---|---|
| KEEP (core/useful, portable) | 108 |
| KEEP_AS_INTERFACE (zero-dep seam) | 7 |
| SIMPLIFY (keep, trim speculative sub-API) | 13 |
| MERGE (fold duplicate into sibling) | 6 |
| **REMOVE** | **58** |

Classifications: ESSENTIAL 71 · USEFUL 50 · DUPLICATE 16 · OVERENGINEERED 29 · HARDWARE_DEP 19 · DEAD 7.

Net effect of executing all tiers: **~70 header files removed/folded (~37% surface reduction)**,
zero loss of core value (async I/O reactor, coroutines, pipelines, HTTP/1.1+2+WS server, SHM IPC,
crypto, memory pools, resilience triad, tracing all retained).

---

## ✅ EXECUTION RECORD (2026-06-14)

Owner decision: execute **A · B · C · E · F + D3 (dev tools) + D4 duplicates**.
**Kept by owner request:** D1 (spatial/game bitsets — `grid_bitset`, `tiled_bitset`, `simd_erasure`,
"will use later") and D2 (distributed-pipeline patterns — `saga`, `canary`, `checkpoint`, `health`,
`idempotency`, `migration`, `slo`).

**Done:**
- Removed **66 files**: Tier A (hardware/third-party: rdma, spdk, ebpf, xdp, pcie, net/nvme_of,
  io/uring_ops, io/ktls, http/fetch_tls, http/http3_client, transport/quiche_transport,
  pipeline/distributed_pipeline, security/pqc), Tier B (reactor/ + protocol/ shims, net/plain_transport
  orphan, grpc/pipeline_integration, http/fetch_pipeline, http/inline_request, middleware/compress+jwt
  shims), Tier C (core/connection, core/mmap_arena, db/simd_parser, http/fetch_stream, http/http2_client,
  security/simd_validator, shm/reliable_cast, shm/topic_schema_registry, tracing/ebpf_guide),
  D3 (all 7 tools/), D4 (middleware/adaptive_rate_limiter).
- Relocated `reactor/micro_ticker.hpp` → `core/micro_ticker.hpp`; repointed 3 includes.
- Removed collateral: 6 example dirs (hardware_io, fetch_stream, http2_client, inspector_dashboard,
  lifecycle_tracing, hardware_chaos) + 2 layout tests (spdk_layout, pcie_layout); trimmed ktls cases
  from security_v15_test.
- Updated umbrella header, root/tests/examples CMake, CLAUDE.md, README.
- Tier E honesty fixes applied (doc-only, safe): smart_cache (in-process scope), connection_pool
  (mutex not lock-free), fetch.hpp (HTTPS out of scope).
- **Module tree: 26 → 17 dirs; headers: 192 → 145.** (Correction: a first pass left 7 Tier-C
  files orphaned-on-disk — `core/connection`, `core/mmap_arena`, `db/simd_parser`, `http/fetch_stream`,
  `shm/reliable_cast`, `shm/topic_schema_registry`, `tracing/ebpf_guide` — because an atomic `git rm`
  aborted on locally-modified siblings. The umbrella already excluded them so the build stayed green;
  they were physically removed in a follow-up pass. Honest total: **72 files deleted.**)
- **Verification: `cmake --build` 0 warnings / 0 errors; `ctest` 20/20 pass; no dangling includes; folder tree tidy.**

**Deliberately NOT done (kept honest, flagged):**
- `core/huge_pages.hpp` — KEPT: it is portable with graceful fallback (new[]/delete[] off-Linux),
  pure OS-feature-with-branching (allowed), and backs the numa_hugepages example. Not hardware-locked.
- `io/io_slice.hpp` — KEPT (restored): redundant with BufferView/iovec in spirit, but actively
  tested/benched/demoed; removing it would break 3 files for ~zero benefit. Future merge candidate.
- Pipeline window/debounce overlap (`stateful_window` ⇄ `windowed_action`,
  `event_actions` ⇄ `stream_ops`) — each has its own example/test; merging risks regression.
  Left as documented future consolidation, not a clean duplicate.
- Tier E Linux-only code trims (io_uring fixed-buffer sub-API removal, numa PerfCounters removal)
  — deferred: cannot be runtime-verified on this ARM Mac; would risk the Linux build blind.

---

## What STAYS — the essential core (untouched)

- **core/**: task, reactor (abstract), dispatcher, epoll_reactor, io_uring_reactor*, kqueue_reactor,
  arena, awaiters, async_logger, cpu_hints, timer_wheel, transport (iface), session_store (iface).
- **pipeline/** (core): action, action_env, concepts, context, async_channel, spsc_channel,
  arena_channel, priority_channel, static_pipeline, dynamic_pipeline, pipeline_graph, stream,
  task_group, service_registry, batch_action, dead_letter, circuit_breaker, retry_policy,
  message_bus, observability, backpressure_monitor, windowed_action, subpipeline_action,
  dynamic_router, pipeline_factory.
- **http/**: parser, request, response, router, fetch, fetch_client, backoff.
- **server/**: connection_handler, http1_handler, http2_handler, websocket_handler, grpc_handler.
- **crypto/** (all 13): sha256, sha512, hmac, hkdf, pbkdf2, chacha20, poly1305, chacha20_poly1305,
  aes_gcm, base64, random, secure_zero, crypto. (AES-GCM is baseline on every target — not exotic.)
- **security/**: simd_jwt, jwt_action.
- **net/**: tcp_stream, tcp_listener, udp_socket, unix_socket, socket_addr, socket_compat,
  uds_advanced, dns, udp_mmsg, udp_multicast.
- **io/**: iovec, scattered_span, read_buf, write_buf, buffer_pool, async_file, socket_opts*.
- **shm/**: shm_channel, shm_bus, shm_compat.
- **buf/**: arena pools — generation_pool, lock_free_hash_map, intrusive_list, inplace_function,
  kqueue_buffer_pool.
- **codec/** (all 4), **middleware/** (cors, rate_limit, request_id, security, content_type,
  static_files, sse, body_encoder, token_auth), **tracing/** (span, trace_context, exporter,
  sampler, trace_logger, lifecycle_tracer*), **config/**, **compat/print**, **db/value**.

(* = SIMPLIFY: keep the header, trim a speculative sub-API — see Tier E.)

---

## TIER A — Hardware-dependent / third-party-required → REMOVE
*Cannot run on Linux-x64/arm64/mac-aarch64 without exotic hardware or a third-party lib. Directly per mandate.*

| Header | Needs to run | Notes |
|---|---|---|
| `rdma/rdma_channel.hpp` | libibverbs + RDMA NIC (mlx5/IB/RoCE) | factory `RDMAContext::open` has no impl |
| `spdk/nvme_io.hpp` | Linux 6.0 io_uring URING_CMD + `/dev/ngXnY` + CAP_SYS_RAWIO | factory unimplemented |
| `ebpf/ebpf_tracer.hpp` | libbpf + CAP_BPF + BTF kernel | also violates A7 (std::function on trace path) |
| `ebpf/memleak_bridge.hpp` | libbpf + CAP_BPF | fully unused |
| `xdp/xsk.hpp`, `xdp/umem.hpp`, `xdp/xdp.hpp` | libbpf/libxdp + AF_XDP NIC driver + CAP_NET_ADMIN | off by default, no consumer |
| `xdp/xdp_pipeline.hpp` | (as above) | **also DEAD/BROKEN** — calls nonexistent methods; would fail to build if `QBUEM_HAS_XDP` ever on |
| `pcie/pcie_device.hpp` (+ `src/pcie/pcie_device.cpp`) | VFIO + IOMMU + vfio-pci device + root | real VFIO code, Linux+root only |
| `pcie/msix_reactor.hpp` | MSI-X PCIe hardware + VFIO + root | methods unimplemented |
| `net/nvme_of.hpp` | RDMA NIC + SPDK/libnvme | pure injection iface, only referenced by chaos tool |
| `io/uring_ops.hpp` | **third-party liburing** (direct `#include`) + kernel 5.19–6.7 | violates D6; unused helper (reactor keeps its own io_uring path) |
| `io/ktls.hpp` | Linux 4.13 kTLS + a userspace TLS lib for keys | unusable end-to-end (no TLS lib in repo) |
| `http/fetch_tls.hpp` | third-party TLS lib (mbedTLS/OpenSSL) + kTLS | does nothing standalone |
| `http/http3_client.hpp` | QUIC/TLS/QPACK via unimplemented injected transport | no example/test/CMake |
| `transport/quiche_transport.hpp` | Rust + Cloudflare quiche | doc-only header; `static_assert(sizeof==0)` stub |
| `pipeline/distributed_pipeline.hpp` | RDMA/InfiniBand multi-host | `listen()` is `co_return {}` no-op; in umbrella despite non-functional |
| `security/pqc.hpp` | third-party PQC lib (liboqs/PQClean) | abstract iface, no crypto impl |
| `core/huge_pages.hpp` | OS-reserved hugepages (`MAP_HUGETLB`) | graceful fallback, but only one niche example — marginal |

**Collateral:** delete `examples/10-hardware/hardware_io/` (calls no real API — only prints usage
strings) + `tests/spdk_layout_test.cpp` + `tests/pcie_layout_test.cpp` (pure struct-layout asserts).
Remove umbrella includes (qbuem_stack.hpp lines ~22-23, 27-29, 63, 75) + CMake stanzas
(`qbuem_pcie` lib, `qbuem_xdp` INTERFACE target, `QBUEM_XDP*` options, hardware_io_example).
**Zero core cross-coupling** — confirmed nothing in core depends on this cluster.

---

## TIER B — Duplicates / forwarding shims → REMOVE / MERGE
*Pure churn; real implementation lives elsewhere.*

| Header(s) | Reality | Action |
|---|---|---|
| `reactor/*.hpp` (16 files) | 1-line `#include <qbuem/core/X.hpp>` ABI-compat shims; header-only lib has no ABI | REMOVE; repoint ~30 example/test includes `qbuem/reactor/*` → `qbuem/core/*`. **Keep `reactor/micro_ticker.hpp`** (only real file there → move to `core/`) |
| `protocol/*.hpp` (protocol.hpp + 4 handlers) | forwarding shims to `server/`; **nothing includes `qbuem/protocol/*`** | REMOVE the `protocol/` dir; `server/` is the single home |
| `net/plain_transport.hpp` | divergent **ODR duplicate** of `transport/plain_transport.hpp`; orphan (included nowhere) | REMOVE net/ copy; keep `transport/plain_transport.hpp` (used by example) |
| `grpc/pipeline_integration.hpp` | superseded by `server/grpc_handler.hpp` (what the grpc example uses) | REMOVE |
| `http/fetch_pipeline.hpp` | re-implements retry+CB already in pipeline/resilience | REMOVE |
| `http/inline_request.hpp` | orphan; `request.hpp` already stores headers inline/zero-alloc | REMOVE |
| `io/io_slice.hpp` | redundant fat-pointer overlapping BufferView/iovec/scattered_span | REMOVE |
| `middleware/compress.hpp` | 1-line shim → `body_encoder.hpp` | fold (low priority) |
| `middleware/jwt.hpp` | 1-line shim → `token_auth.hpp` | fold (low priority) |

---

## TIER C — Dead / unused orphans → REMOVE
*No consumers anywhere; some would link-fail or are broken.*

| Header | Why |
|---|---|
| `core/connection.hpp` | `qbuem::Connection` RAII class — zero references; superseded by net/ stream types |
| `core/mmap_arena.hpp` | zero instantiations; soft libnuma dep; overlaps arena.hpp |
| `db/simd_parser.hpp` | declares out-of-line members with **no `.cpp`** → link-fail if used; zero includers |
| `http/fetch_stream.hpp` | dead; its only example is disabled (targets removed API) |
| `http/http2_client.hpp` | real HTTP/2 is server-side `server/http2_handler.hpp`; example is a broken orphan |
| `security/simd_validator.hpp` | orphan (umbrella-only); result/config use std::vector/std::string (violates own zero-alloc claim); overlaps qbuem-json |
| `shm/reliable_cast.hpp` | speculative HFT feature, zero coverage, x86-only pause spin |
| `shm/topic_schema_registry.hpp` | speculative schema layer, zero consumers, stores std::function validators |
| `tracing/ebpf_guide.hpp` | pure prose comment (zero compilable code) describing external bpftrace |

---

## TIER D — Over-engineered (outside WAS/IPC/pipeline core value) → REMOVE
*Portable, but not core. Owner sanctioned "aggressive" cuts. Sub-grouped so you can pick.*

**D1 — Spatial/game/storage (clearly out of scope for a WAS/IPC library):**
- `buf/grid_bitset.hpp` (52 KB), `buf/tiled_bitset.hpp` (29 KB), `buf/simd_erasure.hpp` (18 KB)
- Collateral: `examples/11-advanced-apps/open_world/`, `examples/11-advanced-apps/spatial_fusion/`,
  `tests/grid_spatial_test.cpp`; `simd_erasure` also touched by `tests/buf_pools_test.cpp`, `tests/neon_validation.cpp`.

**D2 — Distributed-systems pipeline patterns (one demo each, no internal consumers):**
- `pipeline/saga.hpp` (distributed-txn compensation), `pipeline/canary.hpp` (traffic rollout),
  `pipeline/checkpoint.hpp` (offset snapshots), `pipeline/health.hpp` (Graphviz/Mermaid export),
  `pipeline/idempotency.hpp` (dedup; doc cites Redis), `pipeline/migration.hpp` (schema-version DLQ),
  `pipeline/slo.hpp` (error-budget; **also dead-wired** — `action.hpp` exposes `Config.slo` but
  `worker_loop` never reads it).
- Collateral: `examples/07-resilience/saga/`, `examples/07-resilience/canary/` + their tests.
- *(Keep `circuit_breaker`, `retry_policy`, `dead_letter` — those ARE the core resilience triad.)*

**D3 — Dev-tooling toys (mostly stubs; 1-2 demo examples each):**
- `tools/chaos_hardware.hpp` (NVMe-oF/RDMA fault testing — premise removed by Tier A),
  `tools/traffic_twin.hpp`, `tools/qbuem_cli.hpp` (non-functional stub),
  `tools/qbuem_inspector.hpp` (server is a stub), `tools/coro_explorer.hpp`,
  `tools/affinity_inspector.hpp` (sysfs reads stubbed to 0), `tools/buffer_heatmap.hpp`.
- Collateral: `inspector_dashboard`, `io_metrics_dashboard`, `hardware_chaos`, `dev_tools` examples.

**D4 — Misc duplicates / niche:**
- `middleware/adaptive_rate_limiter.hpp` — unused; spawns bg /proc/stat poller; heavier dup of `rate_limit`.
- `http/template_engine.hpp` — HTML templating, not core value.
- `pipeline/stateful_window.hpp` — overlaps `windowed_action`; uses thread_local (forbidden for coroutines) → MERGE into windowed_action.
- `pipeline/event_actions.hpp` ⇄ `pipeline/stream_ops.hpp` — same debounce/throttle/window twice in two paradigms → keep ONE (recommend `stream_ops`), MERGE the other.

---

## TIER E — SIMPLIFY (keep header, trim speculative sub-API)

| Header | Trim |
|---|---|
| `core/io_uring_reactor.hpp` | drop unused fixed-buffer/buf-ring API (kernel 5.19+, no coverage); keep core reactor |
| `core/numa.hpp` | drop `PerfCounters` (privileged perf_event_open + broken include-in-fn-body); keep portable affinity helpers |
| `db/connection_pool.hpp` | fix mislabel ("LockFree O(1)" but real path is mutex+vector) — implement or rename |
| `db/smart_cache.hpp` | strip RDMA/SHM vaporware doc; it's a portable in-process seqlock cache — present it honestly |
| `db/driver.hpp` | keep as zero-dep interface (concrete impl is app-level by design) |
| `io/direct_file.hpp` | keep WAL value; note O_DIRECT is Linux-only with fallback |
| `io/zero_copy.hpp` | keep portable `sendfile()`; trim splice/MSG_ZEROCOPY/erasure extras |
| `net/rudp_socket.hpp` | hand-rolled reliability is niche for a WAS — keep or demote |
| `tracing/lifecycle_tracer.hpp` | strip misleading SHM/OTLP-sidecar/io_uring doc; it's a portable in-process ring |
| `pipeline/action.hpp` | remove dead `slo.hpp` include + `Config.slo` field |
| `buf/inplace_function.hpp` | doc claims it backs reactor/pipeline callbacks but **nothing uses it** — either wire it into the std::function sites or keep as the intended zero-alloc primitive |

---

## TIER F — KEEP_AS_INTERFACE (correct zero-dep seams, do not touch)
`core/session_store.hpp` · `core/transport.hpp` · `db/driver.hpp` ·
`middleware/body_encoder.hpp` · `middleware/token_auth.hpp`

---

## Open question flagged for owner
- **liburing**: `core/io_uring_reactor.hpp` (Linux reactor, KEEP) links optional `liburing` via CMake
  `find_library` with graceful epoll fallback. Strictly that's a third-party lib. If you want *pure*
  zero-dep, the io_uring path can be rewritten on raw `io_uring_setup`/mmap syscalls (larger change).
  Recommendation: **keep liburing as an optional Linux build dep** (it's the canonical io_uring API,
  and epoll is the zero-dep fallback) — but calling this out for your judgment.
