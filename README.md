# qbuem-stack

**Zero Latency · Zero Allocation · Zero Dependency**

> **Current Version: v3.4.0** — Spatial radius queries · `TiledBitset` infinite dynamic world · `IntrusiveList` move semantics · example build fixes.
>
> High-performance C++ infrastructure for Web Application Servers (WAS), Inter-Process Communication (IPC), and Data Processing.

---

## Core Principles

| Principle | Implementation Strategy |
| :--- | :--- |
| **Zero Latency** | SIMD HTTP Parser · `io_uring`/`kqueue` Batching · `sendfile(2)` · `kTLS` |
| **Zero Allocation** | Per-request Arena · `FixedPoolResource` for event entries · Lock-free RingBuffers |
| **Zero Dependency** | Kernel-native syscalls only · No external library dependencies in public headers |
| **Shared-Nothing** | Thread-per-core architecture · NUMA-aware pinning · Minimal cross-thread sync |

---

## ⚡ Performance Truth (The "Hard" Targets)

| Metric | Target Value | Verification Method |
| :--- | :--- | :--- |
| **IPC Latency** | < 200ns (P99.9) | `bench_shm_latency` |
| **HTTP Throughput** | > 10M RPS (Native) / > 40M (Bypass) | `wrk2` / `fb-http-bench` |
| **Event Loop Jitter** | < 5μs | `MicroTicker` built-in metrics |
| **Heap Allocation** | **Strictly 0 Bytes** in hot-paths | `valgrind --tool=massif` |
| **Context Switches** | Near Zero (via Batching) | `perf stat -e context-switches` |

---

## Documentation Map

- **[Roadmap & Progress](./TODO.md)**: Current status and 40M RPS / 200ns benchmark targets.
- **[C++23 Modernization](./docs/cpp23-upgrade-strategy.md)**: Roadmap for migrating to pure C++23 standard types (`std::expected`, `std::jthread`, `std::print`).
- **[ARM NEON Optimization](./docs/neon-optimization-strategy.md)**: SIMD acceleration strategy for AArch64 (Erasure Coding, Cryptography, Masking).
- **[Strategic Vision](./docs/strategic-evaluation.md)**: Why `qbuem-stack` is a universal platform.
- **[Feature Status & Advancement](./docs/feature-status-advancement.md)**: Detailed audit and competitive analysis against industry leaders.

### 🚀 Engineering Standards — "Extreme" Definition of Done (DoD)

To maintain world-class performance, every PR must satisfy these quantifiable benchmarks:

### 1. Latency & Throughput Targets
- **IPC Latency (SHM)**: P99.9 < 200ns.
- **Reactor Dispatch**: O(1) jump via `udata`/`fixed_files` (No map lookups).
- **HTTP Throughput**: > 10M RPS (Standard) / > 40M RPS (AF_XDP Bypass).

### 2. Zero-Everything Principle
- **Zero Allocation**: 0 bytes allocated in the hot loop (Verified via `valgrind --tool=massif` or `jemalloc` stats).
- **Zero Copy**: Data remains in registered buffers (`io_uring` fixed / RIO registered) from NIC to User-space.
- **Zero Lock**: No `std::mutex` or `std::shared_ptr` in handlers. Use SPSC RingBuffers and `GenerationPool`.

### 3. Hardware Alignment
- **NUMA-Aware**: Memory must be allocated on the same NUMA node as the processing core.
- **L3 Locality**: Hardware interrupts (MSI-X) pinned to reactor cores.
- **SIMD-First**: All parsing and encryption must use AVX-512 / AES-NI / ISA-L.

---

## 🛠 AI-Ready Implementation Map
*For AI agents and developers implementing features from the roadmap:*

| Area | Reference Design | Key Techniques |
| :--- | :--- | :--- |
| **Observability** | [Observability Suite](./docs/observability-suite.md) | OTLP, eBPF memleak, qbuem-inspector |
| **Windows** | [Windows Support](./docs/windows-support.md) | IOCP, Registered I/O (RIO), Named Pipes |
| **Security/TLS** | [Security & TLS](./docs/security-tls.md) | kTLS, Hardware Offload, AES-NI, SIMD JWT |
| **Crypto Primitives** | [Crypto Primitives](./docs/crypto-primitives.md) | SHA-256/512, HMAC, PBKDF2, HKDF, Base64, ChaCha20-Poly1305, AES-GCM |
| **Hardware/PCIe** | [PCIe Optimization](./docs/pcie-optimization-guide.md) | VFIO, P2PDMA, CXL, MSI-X Affinity |
| **Storage / File**| [Storage Optimization](./docs/storage-optimization-guide.md) | `O_DIRECT`, `copy_file_range`, SPDK |
| **Legacy Linux** | [epoll Optimization](./docs/epoll-optimization-guide.md) | `EPOLLET`, `EPOLLONESHOT`, `pwait2` |
| **Network (Linux)** | [Network Optimization](./docs/network-optimization-guide.md) | `io_uring` Fixed Buffers, Multishot, AF_XDP |
| **Network (macOS)** | [kqueue Optimization](./docs/kqueue-optimization-guide.md) | `udata` O(1) Dispatch, Changelist Batching |
| **High-Performance Core** | [Primitives Guide](./docs/high-performance-primitives.md) | Lock-Free Map, GenerationPool, MicroTicker |
| **Ecosystem & WAS** | [Ecosystem Expansion](./docs/ecosystem-expansion.md) | Zero-copy Templates, ReliableCast |

---

## Quick Start

```cpp
#include <qbuem/qbuem_stack.hpp>

int main() {
    qbuem::App app;

    // Direct performance handler
    app.get("/hello", [](const qbuem::Request& req, qbuem::Response& res) {
        res.status(200).body("Zero allocation hello");
    });

    // C++23 Coroutine Async handler
    app.get("/api/v1/user/:id", [](const qbuem::Request& req, qbuem::Response& res)
        -> qbuem::Task<void> {
        auto id = req.param("id");
        res.status(200).body("{\"id\":\"" + std::string(id) + "\"}");
        co_return;
    });

    return app.listen(8080) ? 0 : 1;
}
```

### Pipeline + IPC Integration

```cpp
#include <qbuem/pipeline/static_pipeline.hpp>
#include <qbuem/pipeline/message_bus.hpp>
#include <qbuem/shm/shm_bus.hpp>

using namespace qbuem;
using namespace qbuem::shm;

// SHMChannel → SHMSource → Pipeline → MessageBusSink → MessageBus
auto pipeline = PipelineBuilder<RawOrder, RawOrder>{}
    .with_source(SHMSource<RawOrder>("trading.raw_orders"))  // SHM input
    .add<ParsedOrder>(stage_parse)
    .add<ValidatedOrder>(stage_validate)
    .with_sink(MessageBusSink<ValidatedOrder>(bus, "validated"))  // MessageBus output
    .build();

pipeline.start(dispatcher);
```

See [examples/06-ipc-messaging/ipc_pipeline](./examples/06-ipc-messaging/ipc_pipeline/README.md) for the complete 5-scenario walkthrough.

---

## Layered Architecture (The 9 Levels)

1. **Foundation**: `result`, `arena`, `crypto`
2. **Async Core**: `task`, `reactor`, `dispatcher`
3. **IO Primitives**: `net`, `buf`, `file`, `shm`
4. **Transport**: `transport`, `codec`, `server`
5. **Web/HTTP**: `http`, `http-server`
6. **Pipeline**: `context`, `channel`, `pipeline`
7. **Extensions**: `graph`, `resilience`, `db-abstraction`, `security-core`
8. **Protocols**: `ws`, `http2`, `grpc`, `db-pg`
9. **Umbrella**: `qbuem`

---

## Feature Support Matrix

| Category | Features |
| :--- | :--- |
| **Network** | TCP/UDP/Unix, `SO_REUSEPORT`, `TCP_FASTOPEN`, `SO_BUSY_POLL`, RDMA/RoCE |
| **Performance** | SIMD HTTP/1.1 Parser, SIMD JSON (via qbuem-json), Direct `kqueue`/`io_uring` access |
| **Pipeline** | Static/Dynamic Pipelines, Hot-swap, DAG Graphs, Message Bus, IPC Bridge |
| **IPC Integration** | `SHMSource<T>`, `SHMSink<T>`, `MessageBusSource<T>`, `MessageBusSink<T>` |
| **Reliability** | Retry, CircuitBreaker, DeadLetterQueue, SLO Tracking, FutexSync |
| **Observability** | W3C TraceContext, Span Exporters (OTLP/Jaeger), SLO Metrics, eBPF |
| **Security** | `kTLS` Tx Offload, SIMD JWT Parser, JwtAuthAction, Hardware Entropy |
| **Advanced** | `AF_XDP` Support, Zero-copy `sendfile`/`splice`, PCIe VFIO, NVMe/SPDK |

---

## Performance Benchmarks (v3.3.0)

> Measured on Ubuntu 24.04 / GCC 13.3 / `-O3 -march=native`. Single-threaded.

### Core Components

| Component | Operation | Result |
| :--- | :--- | :--- |
| **Arena** | 64B bump-alloc | **2.6 ns** / alloc |
| **Arena** | Request lifecycle (10 alloc + reset) | **1.3 ns** / req |
| **FixedPool** | alloc + dealloc (free-list) | **1.2 ns** / round-trip |
| **AsyncChannel** | try_send + try_recv (MPMC) | **47M ops/s** |
| **SpscChannel** | try_send + try_recv (wait-free) | **113M ops/s** |
| **SpscChannel** | Batch fill 1000 + drain 1000 | **271M ops/s** / **1 GB/s** |
| **HTTP Parser** | GET (74 B) | **320 MB/s** |
| **HTTP Parser** | POST + 10 headers (310 B) | **303 MB/s** |
| **HTTP Parser** | Large headers (542 B, 13 headers) | **355 MB/s** |
| **Router** | Static route lookup | **120 ns** |
| **Router** | Param route (`/users/:id`) | **147 ns** |
| **Router** | 1100-route table lookup | **143 ns** |
| **Context** | `get<T>()` (type-key lookup) | **35–50 ns** |
| **IOSlice** | create + to_iovec() | **0.3 ns** |
| **SHMChannel** | IPC latency | **< 150 ns** (inter-process) |

### GridBitset — Spatial Bitset (game / simulation)

> 256×256 grid, 32 layers, ~10 % density. Single atomic operation unless noted.

| Operation | Result | Notes |
| :--- | :--- | :--- |
| `test(x, y, layer)` | **~28 ns** | L2/L3 latency at 256×256 scale |
| `any_in_range(x, y, 0, 15)` | **~28 ns** | single AND+cmp, L3-bound |
| `any_in_column(x, y)` | **~28 ns** | zero-compare only |
| `lowest_layer / highest_layer` | **~28 ns** | BSF / CLZ instruction |
| `count_layers (POPCNT)` | **~28 ns** | single POPCNT |
| `toggle(x, y, layer)` | **~31 ns** | fetch_xor |
| `any_in_box 8×8` | **37 ns** | 64-cell scan |
| `any_in_box 16×16` | **29 ns** | early-exit on hit |
| `raycast` (32-step DDA) | **30 ns** | early-hit cache-friendly |
| `raycast` (128-step diagonal) | **30 ns** | early-hit stops quickly |
| `for_each_set` (256×256, 10 % sparse) | **~1 ns / cell** | sequential, L1-hot |
| `merge_from` (OR full grid) | **5.5 ns / cell** | 256×256 sequential write |
| `diff_from` (AND-NOT full grid) | **5.3 ns / cell** | 256×256 sequential write |
| `GridBitset2D::for_each_set` (sparse) | **~0.2 ns / cell** | SB-skip + POPCNT |
| `GridBitset2D::raycast_2d` (diagonal) | **29 ns** | Bresenham, early-hit |

---

## GridBitset & TiledBitset — Spatial Index

`GridBitset<W, H, D>` and `GridBitset2D<W, H>` are zero-allocation, wait-free
spatial bitsets for fixed-size worlds. `TiledBitset<TileW, TileH, D>` extends
this to an **infinite dynamic world** using `int64_t` world coordinates.

### Dimension Modes

| Type | Mode | Storage per cell | Use case |
| :--- | :--- | :--- | :--- |
| `GridBitset<W,H,D>` (D=1) | 2D flag | 1 bit packed | Walkability map |
| `GridBitset<W,H,D>` (D≤64) | 2.5D layers | 1 `uint64_t` | Multi-floor, voxel chunk |
| `GridBitset2D<W,H>` | 2D Morton | 8×8 Super-Blocks | Tile map, collision, FOV |
| `TiledBitset<TW,TH,D>` | Infinite tiles | `GridBitset<TW,TH,D>` per tile | Open-world, GIS, robotics |

### API Quick Reference

```cpp
// 2.5D voxel chunk — 256×256 grid, 32 vertical layers
GridBitset<256, 256, 32> world;

// ── Basic writes ────────────────────────────────────────────
world.set(x, y, layer);           // mark occupied
world.clear(x, y, layer);         // mark free
world.toggle(x, y, layer);        // XOR flip (doors, switches) → bool
world.store_column(x, y, mask);   // replace all 32 layers at once
world.clear_box(x1,y1, x2,y2, 0, 31);  // blast damage zone

// ── Queries (~1–3 ns) ───────────────────────────────────────
world.test(x, y, layer);                // single bit
world.any_in_column(x, y);             // broad-phase: anything here?
world.any_in_range(x, y, 2, 5);        // floors 2–5 occupied?
world.any_in_box(x1,y1,x2,y2, 0, 15); // bounding-box overlap
world.lowest_layer(x, y);              // BSF: ground floor
world.highest_layer(x, y);            // CLZ: ceiling floor
world.count_layers(x, y);             // POPCNT: stacked objects

// ── Raycasting (DDA Bresenham) ──────────────────────────────
auto hit = world.raycast(0, 0, /*dx*/1, /*dy*/0, 0, 15, /*max_steps*/128);
if (hit) std::print("hit at ({},{}) after {} steps\n",
                    hit->x, hit->y, hit->steps);

// ── Iteration (sparse skip) ─────────────────────────────────
world.for_each_set([](uint32_t x, uint32_t y, uint64_t layers) {
    // called only for non-empty cells; layers = full layer bitmask
});
world.for_each_set_in_box(x1,y1, x2,y2, 0, 15, fn);

// ── Radius queries (v3.4.0) — per-row sqrt + AVX2/NEON scan ─
world.any_in_radius(cx, cy, r, 0, 15);    // true if any set bit within radius
world.count_in_radius(cx, cy, r, 0, 15);  // count set bits in circle
world.for_each_set_in_radius(cx, cy, r, 0, 15,
    [](uint32_t x, uint32_t y, uint64_t layers) { /* ... */ });

// ── Grid algebra ────────────────────────────────────────────
world.merge_from(other);       // OR  — spawn entities into world
world.intersect_with(other);  // AND — shared visibility
world.diff_from(other);        // AND-NOT — remove destroyed obstacles

// ── 2D tile map (Morton-code cache-optimal layout) ──────────
GridBitset2D<128, 128> tiles;
tiles.set(x, y);              tiles.clear(x, y);
tiles.test(x, y);
tiles.any_in_box(x1,y1, x2,y2);
tiles.count_all();                  // POPCNT entire map
auto los = tiles.raycast_2d(px, py, tx, ty);  // line-of-sight
tiles.for_each_set([](uint32_t x, uint32_t y) { /* ... */ });

// ── TiledBitset — infinite dynamic world (v3.4.0) ───────────
// int64_t world coordinates; tiles allocated on demand
TiledBitset<256, 256, 16> open_world;

open_world.set(wx, wy, layer);            // creates tile if needed
open_world.clear(wx, wy, layer);
bool hit = open_world.test(wx, wy, layer);

// Cross-tile spatial queries
open_world.any_in_box(wx1, wy1, wx2, wy2, 0, 15);
open_world.any_in_radius(cx, cy, r, 0, 15);
open_world.count_in_radius(cx, cy, r, 0, 15);
open_world.raycast(wx, wy, dx, dy, 0, 15, max_steps);

// Memory management
size_t freed = open_world.evict_empty_tiles();  // release fully-empty tiles
open_world.for_each_tile([](int32_t tx, int32_t ty, const auto& tile) {
    // iterate all loaded tiles
});
auto stats = open_world.memory_stats(); // {tile_count, allocated_bytes}
```

### Thread Safety

**`GridBitset` (fixed-size):**
All read operations (`test`, `any_*`, `count_*`, `for_each_*`, `raycast*`) are
**wait-free** — safe to call from any number of threads concurrently with no locks.
All write operations (`set`, `clear`, `toggle`, `merge_from`, `diff_from`,
`clear_box`) are **lock-free** via `fetch_or`/`fetch_and`/`fetch_xor`.

**`TiledBitset` (infinite):**
Uses a per-thread 4-slot TLS cache for hot-path tile lookups (mutex-free).
A `shared_mutex` is only acquired on a TLS cache miss (cold path). Tile creation
uses a `unique_lock`. Safe for concurrent reads and writes from multiple threads.

---

## Examples

The [`examples/`](./examples/) directory contains **58 programs** organized into 11 categories, each with a detailed README.

| # | Category | Programs | Highlights |
| :--- | :--- | :---: | :--- |
| [01](./examples/01-foundation/) | Foundation | 3 | `hello_world`, `async_timer`, `micro_ticker` |
| [02](./examples/02-network/) | Network | 7 | TCP echo, UDP advanced, Unix socket, WebSocket, HTTP fetch, HTTP/2, FetchStream |
| [03](./examples/03-memory/) | Memory | 4 | Arena, zero-copy, NUMA + huge pages, lock-free bench |
| [04](./examples/04-codec-security/) | Codec & Security | 4 | Length-prefix/line codecs, crypto URL utilities, security middleware, **crypto primitives** (SHA/HMAC/PBKDF2/HKDF/AEAD) |
| [05](./examples/05-pipeline/) | Pipeline | 12 | Fan-out, hot-swap, batching, dynamic router, backpressure, stateful window, windowed action |
| [06](./examples/06-ipc-messaging/) | IPC & Messaging | 4 | SHMChannel, **flagship IPC pipeline**, MessageBus, SPSC |
| [07](./examples/07-resilience/) | Resilience | 6 | Retry + CircuitBreaker + DLQ, Saga, Canary, Checkpoint, SLO |
| [08](./examples/08-observability/) | Observability | 5 | W3C tracing, lifecycle tracer, inspector dashboard, TimerWheel, TaskGroup |
| [09](./examples/09-database/) | Database | 2 | Connection pool, session store, coroutine JSON |
| [10](./examples/10-hardware/) | Hardware | 2 | PCIe VFIO + RDMA + eBPF + NVMe (Linux), kqueue (macOS) |
| [11](./examples/11-advanced-apps/) | Advanced Apps | 10 | Autonomous driving, HFT matching, open-world spatial, trading platform, game server, hardware chaos |

**Recommended learning path:**
```
hello_world → async_timer → tcp_echo_server → arena → pipeline/fanout
    → ipc_pipeline → resilience → trading_platform
```

---

## Roadmap Highlights

- **v3.3.0**: Full C++23 enforcement (`std::print`, `std::jthread`, `std::format_to_n`). ARM NEON SIMD parity (WebSocket masking, GF(2⁸) erasure, base64url, CRC32, HTTP header scan). `ConfigManager` + `Secret<T>`. GridBitset game-ready APIs (toggle, raycast, for_each, merge/diff/intersect).
- **v3.4.0 (Current)**: `GridBitset` radius queries (`any_in_radius`, `count_in_radius`, `for_each_set_in_radius`) with per-row sqrt + AVX2/NEON. `TiledBitset<W,H,D>` infinite dynamic world with TLS-cached tile lookup. `IntrusiveList` move semantics. Build fixes across `lockfree_bench`, `hft_matching`, `micro_ticker_example`. 58 examples total.
- **v3.5.0 (Next)**: AF_XDP production examples, QUIC/HTTP3 via quiche integration, sanitizer clean-up pass.

---

*qbuem-stack — Built for mechanical sympathy and extreme performance.*
