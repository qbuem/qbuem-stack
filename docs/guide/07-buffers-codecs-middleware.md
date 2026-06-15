# Buffers, Codecs & Middleware

This section documents three closely related parts of qbuem-stack:

- **`<qbuem/buf/*>`** — zero-allocation data-structure primitives (object pools, lock-free maps, intrusive lists, an inline callable) plus a family of cache-aware **spatial bitsets** (`GridBitset` / `TiledBitset`) for robotics / game / GIS workloads, and a SIMD **erasure coder** for data redundancy.
- **`<qbuem/codec/*>`** — protocol *framing* over a raw byte stream: a common `IFrameCodec<Frame>` interface plus three concrete codecs (length-prefix, line, HTTP/1.1).
- **`<qbuem/middleware/*>`** — composable HTTP request/response middleware for the `App`/`Router` (CORS, rate limiting, security headers, request IDs, content-type enforcement, static files, SSE), plus two **zero-dependency interface points** (`IBodyEncoder`, `ITokenVerifier`) where the application injects compression and token-verification logic.

Everything here obeys the four pillars: error values via `std::expected` (aliased `Result<T>` = `std::expected<T, std::error_code>`), no exceptions on hot paths, `std::span` / `std::string_view` views instead of copies, and zero third-party includes.

> **Type aliases used throughout** (from `<qbuem/common.hpp>`):
> ```cpp
> template <typename T> using Result          = std::expected<T, std::error_code>;
> template <typename E> using unexpected       = std::unexpected<E>;
> using BufferView        = std::span<const uint8_t>;   // immutable byte view
> using MutableBufferView = std::span<uint8_t>;
> ```

---

## Part 1 — Buffers (`<qbuem/buf/*>`)

### Catalog

| Type | Header | Role | Thread model |
|------|--------|------|--------------|
| `GenerationPool<T>` / `GenerationHandle` | `buf/generation_pool.hpp` | ABA-safe lock-free fixed-capacity object pool, versioned handles | MPMC lock-free |
| `LockFreeHashMap<K, V>` | `buf/lock_free_hash_map.hpp` | Open-addressed wait-free hash map (small trivially-copyable V) | MPMC lock-free |
| `IntrusiveList<T>` / `IntrusiveNode` | `buf/intrusive_list.hpp` | Doubly-linked list with pointers embedded in the object (no heap) | Single-thread / externally synchronized |
| `inplace_function<Sig, N, A>` | `buf/inplace_function.hpp` | Small-buffer type-erased callable, **never heap-allocates** | Move-only value type |
| `KqueueBufferPool` | `buf/kqueue_buffer_pool.hpp` | Cache-aligned user-space buffer ring for macOS kqueue I/O | Mutex-guarded (cold-path acquire/release) |
| `GridBitset<W, H, D>` / `GridBitset2D<W, H>` | `buf/grid_bitset.hpp` | Fixed-size cache-aware spatial bitset (2D / 2.5D), wait-free reads | MPMC lock-free per-cell |
| `TiledBitset<TileW, TileH, D>` | `buf/tiled_bitset.hpp` | Unbounded spatial bitset built from `GridBitset` tiles on demand | Lock-free per cell; tile creation takes a `unique_lock` |
| `ErasureCoder` + `gf256::*` | `buf/simd_erasure.hpp` | Reed-Solomon erasure coding over GF(2⁸) with SIMD GF-multiply | Read-only after construction; `encode()`/`reconstruct()` re-entrant |

> All of these compile on Linux x86_64, ARM64 boards (Jetson-class), and Mac aarch64. SIMD-accelerated paths in `GridBitset`/`simd_erasure` detect AVX-512 / AVX2 / SSE4.2 on x86 and NEON on ARM at compile time, with a portable scalar fallback otherwise — the API is identical on every platform.

---

### 1.1 `GenerationPool<T>` — ABA-safe lock-free object pool

**What it is.** A fixed-capacity pool of `T`-sized slots. Instead of returning bare pointers (which suffer the ABA problem under concurrent reuse), `acquire()` returns a **`GenerationHandle`** — a 64-bit value packing a slot index (low 32 bits) and a generation counter (high 32 bits). A handle only resolves to a live pointer while its generation matches the slot; stale handles resolve to `nullptr`.

**When to use it.** Per-message / per-event objects in a hot path where you would otherwise `make_shared<Event>()` or `new Event` — connection contexts, order records, packet descriptors. Use it when one thread may free a slot while another still holds a (possibly stale) reference; the generation check makes use-after-free *safe* (returns `nullptr`) instead of UB. The flagship HFT example stores its order book in one (`examples/11-advanced-apps/hft_matching/`).

**When NOT to use it.** If objects are strictly request-scoped and freed all at once, use an `Arena` (`<qbuem/buf/arena.hpp>`) and `reset()` — cheaper than per-slot acquire/release. If you don't need ABA safety / handle indirection, a `FixedPoolResource<T, N>` is simpler.

**How to use it** (signatures copied from the header):

```cpp
#include <qbuem/buf/generation_pool.hpp>
using namespace qbuem;

struct Event { uint64_t timestamp; int type; };

GenerationPool<Event> pool(256);            // capacity must be >= 1 and < UINT32_MAX

// acquire() -> std::optional<AcquireResult{ GenerationHandle handle; T* ptr; }>
if (auto ar = pool.acquire()) {
    new (ar->ptr) Event{.timestamp = 42, .type = 1};   // placement-new: ptr is RAW storage
    GenerationHandle h = ar->handle;

    // Elsewhere / later — resolve is wait-free, returns nullptr if stale:
    if (Event* e = pool.resolve(h)) {
        // ... use *e ...
    }
    pool.release(h);                          // return slot; stale/double release = safe no-op
} else {
    // pool exhausted (std::nullopt)
}
```

`GenerationHandle` is a trivially-copyable 32+32-bit value: `h.index()`, `h.gen()`, `h.valid()`, default-constructs to the null handle (`GenerationHandle::kNull == UINT64_MAX`). You can reconstruct one with `GenerationHandle{index, gen}` — the HFT example does exactly this when releasing by raw index+generation.

**Gotchas / constraints.**
- `T` must be **default-constructible** (`static_assert`) — the pool pre-allocates `capacity` slots at construction.
- `acquire()` returns **uninitialized storage**. You *must* placement-new into `ar->ptr`, and you are responsible for calling `T`'s destructor before `release()` if `T` is non-trivial.
- Capacity is fixed at construction — there is no growth. `acquire()` returns `std::nullopt` when full.
- The free list and head pointer are `alignas(64)` to avoid false sharing; the pool is non-copyable.

---

### 1.2 `LockFreeHashMap<K, V>` — wait-free open-addressed map

**What it is.** An MPMC lock-free hash map using open addressing with linear probing. Each slot is `alignas(64)` and walks `Empty → Busy → Committed → Tombstone` via atomic CAS. Capacity is rounded up to the next power of two.

**When to use it.** Hot-path lookups keyed by an integer where you'd otherwise reach for `std::mutex + std::unordered_map` — symbol → order-book index, fd → connection slot, session-id → handle. The benchmark in `examples/03-memory/lockfree_bench/` measures it vs. a mutex-guarded `unordered_map`.

**When NOT to use it.** If `V` is larger than 8 bytes or non-trivially-copyable (it must fit a `std::atomic<V>`), store a small handle/index instead and keep the payload in a `GenerationPool`. If the map is single-threaded, plain `std::unordered_map` (off the hot path) is fine.

**How to use it:**

```cpp
#include <qbuem/buf/lock_free_hash_map.hpp>
using namespace qbuem;

LockFreeHashMap<uint64_t, uint32_t> map(1024);   // capacity rounded up to power of two

bool ok = map.put(42, 100);          // [[nodiscard]] -> false if the map is full
if (!ok) { /* map saturated */ }

Result<uint32_t> v = map.get(42);    // Result<V> = expected<uint32_t, error_code>
if (v) {
    uint32_t value = *v;
} else {
    // v.error() == std::errc::no_such_file_or_directory  (key not present)
}

bool removed = map.remove(42);       // [[nodiscard]] -> tombstones the slot
```

**Gotchas / constraints.**
- `K` and `V` must be **trivially copyable**; `sizeof(V) <= 8` (asserted). `K` is hashed as `uint64_t`.
- **Key `0` is reserved** as the empty sentinel — never `put`/`get`/`remove` key 0 (debug `assert`).
- `put()`, `get()`, `remove()` are `[[nodiscard]]` — handle the boolean / `Result`.
- No resize: when full, `put()` returns `false`. Size the capacity generously (load factor matters for probe length).
- `remove()` leaves tombstones, which lengthen probe chains; a churny workload may degrade over time — recreate the map if you delete heavily.

---

### 1.3 `IntrusiveList<T>` — zero-allocation doubly-linked list

**What it is.** A circular doubly-linked list whose `prev`/`next` pointers live **inside** the stored object (derive from `IntrusiveNode`). No node wrappers are allocated — the list only manipulates embedded pointers, so it is fully zero-allocation. O(1) push/pop/remove, and O(1) removal given only the object pointer.

**When to use it.** Ready queues, timer queues, LRU chains, free lists — any list where the elements already have stable storage (a pool, an array, an arena) and you want `std::list` semantics with zero heap traffic. `examples/03-memory/lockfree_bench/` and the HFT order book both use it.

**When NOT to use it.** If you don't control the element type (can't add a base class), or elements must live in multiple unrelated containers without extra base classes, fall back to `std::list`/`std::deque` off the hot path.

**How to use it:**

```cpp
#include <qbuem/buf/intrusive_list.hpp>
using namespace qbuem;

struct Task : public IntrusiveNode { int priority; };

IntrusiveList<Task> ready;            // requires T derived from IntrusiveNode
Task a, b;                            // storage owned by the caller (stack/pool/arena)

ready.push_back(&a);                  // O(1); node must NOT already be linked
ready.push_front(&b);

for (Task& t : ready) { /* range-for: bidirectional iterator */ }

Task* head = ready.pop_front();       // [[nodiscard]] O(1)
ready.remove(&b);                     // O(1) given just the pointer
bool empty = ready.empty();
size_t n   = ready.size();            // O(n) — counts by walking
```

**Gotchas / constraints.**
- **Lifetime is your responsibility**: a node must be removed from the list *before* its storage is destroyed. A dangling pointer left in the list is UB.
- `push_back`/`push_front` assert the node is **not already linked** (a node can only be in one list per base class). To put one object in multiple lists, inherit `IntrusiveNode` multiple times via distinct base classes.
- `size()` is O(n) (it walks the chain) — prefer `empty()` in hot code.
- Not internally synchronized — guard concurrent mutation yourself.

---

### 1.4 `inplace_function<Signature, Capacity, Align>` — zero-alloc callable

**What it is.** A drop-in alternative to `std::function` that stores the target callable in an inline `Capacity`-byte buffer and **never heap-allocates** — a closure that doesn't fit is a *compile error*, not a hidden `malloc`. It is **move-only**.

**When to use it.** Anywhere a `std::function` would sit on a hot path: reactor event callbacks, pipeline stage storage, timer callbacks (per `A7`, `std::function` is forbidden on hot paths because it heap-allocates closures larger than ~16 bytes). The default `Capacity` is 48 bytes.

**When NOT to use it.** If you need copyability, shared ownership of the callable, or arbitrarily large captures, use `std::function` on a cold path. If the callable is a plain function pointer or stateless lambda, a raw function pointer is even leaner.

**How to use it:**

```cpp
#include <qbuem/buf/inplace_function.hpp>
using namespace qbuem;

// Signature template arg; Capacity defaults to 48, Align to alignof(max_align_t).
inplace_function<void(int)> cb = [handle, this](int fd) { resume(fd, handle); };
cb(fd);                              // indirect call, zero allocation, ever

if (cb) { /* explicit operator bool */ }
cb.reset();                          // destroys the target, becomes empty

// Need a bigger inline buffer? Widen N:
inplace_function<int(std::string_view), 96> big = /* large-capture lambda */;
```

**Gotchas / constraints.**
- The target must satisfy `sizeof(F) <= Capacity` and `alignof(F) <= Align` and be **nothrow-move-constructible** — all enforced by `static_assert` with clear messages ("callable too large… raise N").
- **Move-only**: copy construction/assignment are deleted. The hot-path idiom is construct → store → move.
- Calling an empty `inplace_function` invokes a null function pointer — check `operator bool` first if it may be empty.

---

### 1.5 `KqueueBufferPool` — macOS user-space buffer ring

**What it is.** A pool of cache-line-aligned (64-byte) buffers carved from a single `posix_memalign` allocation, mimicking an `io_uring` buffer ring in user space for **macOS kqueue** I/O. Each buffer carries an address, length, and a `uint16_t` id for release.

**When to use it.** macOS reactors (`kqueue`) that want pre-registered, false-sharing-free read buffers — Linux uses `io_uring` fixed buffers instead. See `examples/10-hardware/kqueue_sophistication/`.

**How to use it:**

```cpp
#include <qbuem/buf/kqueue_buffer_pool.hpp>
using namespace qbuem;

KqueueBufferPool pool(4096, 256);    // 256 buffers, each rounded up to a 64-byte multiple

KqueueBufferPool::Buffer b = pool.acquire();   // O(1)
if (b.addr) {                                   // addr == nullptr means pool empty
    // read(fd, b.addr, b.len) ...
    pool.release(b.bid);                         // return by buffer id
}
size_t free = pool.available();
```

**Gotchas / constraints.**
- `acquire()`/`release()` take a `std::mutex` — designed for the cold setup/teardown of buffer registration, not the per-byte hot path (the read itself is zero-copy into the buffer).
- The constructor `throw`s `std::bad_alloc` if `posix_memalign` fails — this is a construction-time concern, not a hot-path one.
- `buffer_size` is rounded up to the next 64-byte multiple; size your reads against `Buffer::len`, not your requested size.

---

### 1.6 Spatial bitsets — `GridBitset`, `GridBitset2D`, `TiledBitset`

These three types form a spatial-occupancy toolkit for robotics (occupancy grids / SLAM), game servers (line-of-sight, aggro), GIS/cartography, EDA, and RF coverage maps. They share an idiom: occupancy bits are packed into `uint64_t` words, **reads are wait-free** (one atomic load), and **writes are lock-free** (`fetch_or`/`fetch_and`/`fetch_xor`). Box / radius / raycast scans dispatch to SIMD (AVX-512 / AVX2 / NEON) with a scalar fallback.

#### 1.6.1 `GridBitset<W, H, D>` — fixed 2.5D grid

**What it is.** A `W×H` grid where each cell stores up to `D ≤ 64` **vertical layers** packed into one `uint64_t`. A layer-range query ("anything between floor 2 and 5 at (x,y)?") is a single AND + compare (~1 ns). `W`, `H`, `D` are compile-time template parameters, so storage is a flat `alignas(64)` array — no heap.

**When to use it.** A bounded world / sensor field of known size: a robot's local cost map, a fixed game level, a chip layout region. For an **unbounded** world use `TiledBitset`. If you only need 2D (D == 1) and want Morton-code cache blocking, use `GridBitset2D` instead.

**How to use it** (real signatures):

```cpp
#include <qbuem/buf/grid_bitset.hpp>
using namespace qbuem;

GridBitset<256, 256, 16> grid;                       // 256x256 cells, 16 vertical layers

// Writes (lock-free, layer in [0, D))
grid.set(10, 20, 3);                                 // occupy layer 3 at (10,20)
grid.clear(10, 20, 3);
grid.set_column(10, 20, 0b1011u);                    // OR a precomputed layer mask
grid.toggle(10, 20, 3);                              // XOR-flip — doors/switches
grid.clear_all(10, 20);                              // clear every layer in the cell

// Point / range reads (wait-free)
bool occ   = grid.test(10, 20, 3);
bool any   = grid.any_in_range(10, 20, 2, 5);        // any layer in [2,5] set?
uint64_t c = grid.snapshot(10, 20);                  // full 64-bit layer bitmap
bool col   = grid.any_in_column(10, 20);             // broad-phase: anything at all?
uint32_t lo = grid.lowest_layer(10, 20);             // BSF — ground detection
uint32_t hi = grid.highest_layer(10, 20);            // ceiling detection
uint32_t k  = grid.count_layers(10, 20);             // POPCNT of the cell

// Box queries (SIMD row scan); corners inclusive, from<=to<D
bool boxhit  = grid.any_in_box(0, 0, 31, 31, 0, 15);
uint32_t cnt = grid.count_in_box(0, 0, 31, 31, 0, 15);

// Iterate occupied cells: fn(uint32_t x, uint32_t y, uint64_t layer_mask)
grid.for_each_set([](uint32_t x, uint32_t y, uint64_t mask) { /* ... */ });

// Set algebra between two grids of the same dimensions
GridBitset<256, 256, 16> other;
grid.merge_from(other);        // OR  — union (spawn / merge zones)
grid.intersect_with(other);    // AND — shared visibility / overlap
grid.diff_from(other);         // AND-NOT — remove destroyed obstacles

// Bresenham DDA raycast — first occupied cell in layer range [from,to]
auto hit = grid.raycast(/*x0*/10, /*y0*/20, /*dx*/1, /*dy*/0,
                        /*from*/0, /*to*/15, /*max_steps*/256);
if (hit) {                                          // std::optional<RayHit>
    // hit->x, hit->y, hit->layer_mask, hit->steps
}
```

**Gotchas / constraints.**
- `D` must be in `[1, 64]` (one `uint64_t` per cell). For pure 2D prefer `GridBitset2D`.
- All index arguments are debug-`assert`ed against bounds — out-of-range coords are UB in release. Box/raycast require `from <= to && to < D` and inclusive corners with `x1<=x2`, `y1<=y2`.
- Non-copyable (the cell array is `alignas(64)` atomics). Concurrent write + read of the **same** cell is safe (atomic); multiple writers to *different* cells are fully safe.
- `set`/`clear` use `memory_order_release`; reads use `acquire` — but `raycast`/`for_each_set` use `relaxed` loads for speed (snapshot-consistent, not linearizable across cells).

#### 1.6.2 `GridBitset2D<W, H>` — Morton-blocked pure-2D bitset

**What it is.** A 2D-only variant that packs cells into 8×8 = 64-bit "Super-Blocks" laid out in **Morton (Z-curve) order**, so spatially nearby cells are nearby in memory and box queries touch the minimum number of cache lines. Uses BMI2 `PDEP`/`PEXT` on x86 when available, portable bit-spreading otherwise.

**When to use it.** A flat 2D obstacle/visibility map (no vertical layers). Choose this over `GridBitset<W,H,1>` when you want the Morton cache-blocking for dense box scans.

```cpp
GridBitset2D<128, 128> obstacles;
obstacles.set(3, 7);
bool blocked = obstacles.test(3, 7);
obstacles.clear(3, 7);

bool boxhit  = obstacles.any_in_box(0, 0, 63, 63);
uint32_t cnt = obstacles.count_in_box(0, 0, 63, 63);
uint32_t all = obstacles.count_all();
obstacles.clear_box(10, 10, 20, 20);

obstacles.for_each_set([](uint32_t x, uint32_t y) { /* ... */ });    // 2-arg callback
auto hit = obstacles.raycast_2d(0, 0, 1, 1, /*max_steps*/128);       // std::optional<RayHit{ x,y,steps }>
```

> Note the callback arity differs from the 2.5D version: `GridBitset2D::for_each_set` passes `(x, y)` only; `GridBitset::for_each_set` passes `(x, y, layer_mask)`. The 2D `RayHit` has `{x, y, steps}` (no `layer_mask`).

#### 1.6.3 `TiledBitset<TileW, TileH, D>` — unbounded spatial bitset

**What it is.** An **unbounded** 2D/2.5D occupancy map. World coordinates are signed `int64_t` (≈ −2⁶³ … 2⁶³−1 on each axis). It lazily allocates fixed-size `GridBitset<TileW, TileH, D>` tiles, indexes them in a `shared_mutex`-protected map, and keeps a 4-slot **thread-local cache** so steady-state access bypasses the mutex entirely (~7–8 ns per op). Box/radius/raycast queries split across tile boundaries transparently.

**When to use it.** Open-world game servers, robotics SLAM occupancy maps, GIS coverage layers, warehouse AMR floor plans — anything where the extent isn't known up front. See `examples/11-advanced-apps/open_world/` (a full game-server tick loop) and `spatial_fusion/`.

**How to use it** (signatures from the header):

```cpp
#include <qbuem/buf/tiled_bitset.hpp>
using namespace qbuem;

TiledBitset<256, 256, 16> world;                     // 256x256 tiles, 16 layers, infinite extent

// Writes — first write to a region creates the tile (one unique_lock, TLS-cached after)
world.set(-100'000, 50'000, 5);                       // negative coords are valid
world.clear(-100'000, 50'000, 5);                     // no-op if tile not loaded
world.set_column(0, 0, 0b1u << 3);
bool now_set = world.toggle(10, 20, 3);

// Reads — unloaded tiles read as empty (false / 0)
bool t  = world.test(10, 20, 3);
bool r  = world.any_in_range(10, 20, 0, 7);
uint64_t snap = world.snapshot(10, 20);

// Multi-tile spatial queries
bool box  = world.any_in_box(-300, -300, 300, 300, 0, 15);
uint32_t n = world.count_in_box(-300, -300, 300, 300, 0, 15);
bool aggro = world.any_in_radius(/*cx*/px, /*cy*/py, /*r*/20, /*from*/0, /*to*/15);

// Cross-tile Bresenham raycast (line-of-sight / bullet trace)
auto hit = world.raycast(/*x0*/-500, /*y0*/0, /*dx*/1, /*dy*/0,
                         /*from*/0, /*to*/15, /*max_steps*/1000);
if (hit) {                                            // std::optional<RayHit{ wx, wy, layer_mask, steps }>
    // hit->wx, hit->wy are WORLD coordinates (int64_t)
}

// Tile lifecycle / stats
world.prefetch_tile(/*tx*/0, /*ty*/0);               // pre-allocate
size_t freed = world.evict_empty_tiles();            // reclaim all-zero tiles (NOT during writes)
size_t tiles = world.loaded_tile_count();
size_t bytes = world.memory_bytes();                 // approximate heap footprint
world.reset_all();                                   // drop everything
```

**Gotchas / constraints.**
- `D ∈ [1, 64]` (asserted). `TileW`/`TileH` should be powers of two for cheap coordinate math; the example uses 256.
- **`evict_empty_tiles()` and `reset_all()` are NOT safe to call concurrently with writes** — they take a `unique_lock` and mutate the tile map. Run them during a quiescent phase (e.g. a cleanup tick).
- Per-cell ops on a *loaded* tile are lock-free; only the **first** write into an unloaded tile pays for one `unique_lock`. Pre-warm with `prefetch_tile` if you need to avoid that latency spike on a hot tick.
- Raycast world deltas `dx`/`dy` are `int32_t` direction components (at least one non-zero); origins are `int64_t` world coordinates.
- Reads of unloaded regions are free and return "empty" — you never need to bounds-check the world.

---

### 1.7 `ErasureCoder` — SIMD Reed-Solomon erasure coding

**What it is.** A Reed-Solomon coder over GF(2⁸): it turns `k` data shards into `m` parity shards such that **any `k` of the `k+m` shards** can rebuild the original. The Galois-Field multiply-accumulate (the inner loop) is SIMD-accelerated — AVX2 (`VPSHUFB` split-table), SSE4.2, or NEON, with a scalar fallback. The header also exposes a small `gf256` namespace (`mul`, `pow`, `inv`, log/antilog tables, `fast_mul`) and the free function `gf_mul_add(coeff, in, out)` for building custom GF pipelines.

**When to use it.** Durable storage / replication with less overhead than full copies (RAID-6-style 2-parity, or RS(10,4) for 40% overhead tolerating 4 losses), or network FEC for lossy links. Zero-dependency: no libisal needed.

**When NOT to use it.** If you only need single-parity and simplicity, plain XOR (RAID-5) is one `gf_mul_add` away. If latency on the *reconstruction* path must be zero-allocation, see the gotcha below.

**How to use it** (signatures from the header):

```cpp
#include <qbuem/buf/simd_erasure.hpp>
using namespace qbuem;

// RS(10, 4): 10 data + 4 parity, tolerates up to 4 lost shards.
ErasureCoder ec(/*k*/10, /*m*/4);                    // k,m in [1,254], k+m<=255
int total = ec.n();                                   // 14;  ec.k()==10, ec.m()==4

// All shards must be EQUAL-sized contiguous byte spans.
std::array<std::span<std::byte>, 14> shards;
// ... point shards[0..9] at your data, shards[10..13] at parity scratch ...

ec.encode(shards);                                    // fills shards[10..13] (parity)

// Simulate 3 losses, then reconstruct.
std::array<bool, 14> present{};  present.fill(true);
present[2] = present[5] = present[11] = false;

Result<void> r = ec.reconstruct(shards, present);     // [[nodiscard]] Result<void>
if (!r) {
    // r.error() == std::errc::not_enough_memory  -> fewer than k shards present
    // r.error() == std::errc::invalid_argument   -> decode sub-matrix singular
} else {
    // shards[2], [5], [11] are restored in place
}
```

**Gotchas / constraints.**
- All shards passed to `encode`/`reconstruct` must have **equal size**; `encode()` overwrites `shards[k .. k+m-1]` with parity. Spans are caller-owned — the coder never allocates the shard buffers.
- `encode()` is `noexcept` and operates only on caller spans (zero-allocation). **`reconstruct()` allocates internally** (a small `std::vector` for the inverted decode matrix and a temporary) — it is *not* on the zero-allocation hot path. Treat reconstruction as a cold/recovery operation; encoding is the hot path.
- The coder is **read-only after construction** — `encode()` and `reconstruct()` are safe to call concurrently from multiple threads on disjoint shard sets.
- `gf256::fast_mul` uses runtime-initialized log/antilog tables (`inline const` globals); `gf256::mul` is `constexpr` (Russian-peasant) if you need compile-time GF math.
- The documented `IErasureBackend` pluggable interface is *not present* in this trimmed header — only the built-in `ErasureCoder` and `gf256`/`gf_mul_add` primitives are available.

---

## Part 2 — Codecs (`<qbuem/codec/*>`)

Codecs sit between a raw byte stream and typed protocol *frames*. They live in namespace `qbuem::codec`.

### 2.1 `IFrameCodec<Frame>` + `DecodeStatus` — the framing interface

**What it is.** The common abstraction every codec implements (`codec/frame_codec.hpp`). `decode()` is **incremental**: feed it whatever bytes you have; it returns `Incomplete` until a whole frame arrives, then `Complete` (advancing the buffer past the consumed bytes). `encode()` serializes a frame into an `iovec` array for a single scatter-gather `writev`.

```cpp
enum class DecodeStatus { Complete, Incomplete, Error };

template <typename Frame>
class IFrameCodec {
public:
  virtual ~IFrameCodec() = default;
  // On Complete, advances `buf` (BufferView& = std::span<const uint8_t>&) past consumed bytes.
  virtual DecodeStatus decode(BufferView& buf, Frame& out) = 0;
  // Returns #iovec entries used (0 = failure). `arena` may be nullptr.
  virtual size_t encode(const Frame& frame, iovec* vecs, size_t max_vecs,
                        std::pmr::memory_resource* arena) = 0;
  virtual void reset() = 0;          // clear decoder state for the next frame
};
```

**The decode loop idiom** (drive it on every `recv`):

```cpp
switch (codec.decode(buf, frame)) {
  case DecodeStatus::Complete:   handle(frame); codec.reset(); break;  // keep-alive: reset for next
  case DecodeStatus::Incomplete: /* recv more bytes, call decode again */ break;
  case DecodeStatus::Error:      close_connection(); break;
}
```

**Gotchas.** `reset()` only clears the **decoder** state; encode state is independent. After an `Error`, you must `reset()` (or discard the codec) before parsing the next frame.

### 2.2 `LengthPrefixedCodec` — 4-byte big-endian length framing

**What it is.** Frames as `[ uint32 big-endian length ][ payload ]`. The frame type is `LengthPrefixedFrame{ uint32_t length; std::vector<std::byte> payload; }` (length is host byte order; the codec converts on the wire). Runnable demo: `examples/04-codec-security/codec/`.

```cpp
#include <qbuem/codec/length_prefix_codec.hpp>
using namespace qbuem;             // for BufferView
using namespace qbuem::codec;

// Encode
LengthPrefixedFrame out;
out.length  = static_cast<uint32_t>(msg.size());
out.payload.assign(reinterpret_cast<const std::byte*>(msg.data()),
                   reinterpret_cast<const std::byte*>(msg.data()) + msg.size());
LengthPrefixedCodec codec;
iovec vecs[2];
size_t n = codec.encode(out, vecs, 2, nullptr);   // vecs[0]=4B header, vecs[1]=payload; n==2 (0 if max_vecs<2)
// ::writev(fd, vecs, n);

// Decode (incremental)
std::vector<uint8_t> wire = /* received bytes */;
LengthPrefixedCodec rx;
LengthPrefixedFrame frame;
BufferView buf{wire.data(), wire.size()};          // std::span<const uint8_t>
if (rx.decode(buf, frame) == DecodeStatus::Complete) {
    // frame.payload holds the decoded body (moved out); buf has advanced
    rx.reset();
}
```

**Gotchas / constraints.**
- Built-in **DoS guard**: a length prefix above `kMaxFrameSize` (64 MiB) returns `DecodeStatus::Error` *before* any allocation — an attacker can't request a multi-GiB buffer with 4 bytes.
- `decode()` accumulates the body in an internal `std::vector` (this codec materializes the payload, so it's not zero-copy — use `LineCodec` when you need a view). On `Complete` the body is **moved** into `out.payload`.
- `encode()` needs `max_vecs >= 2` or it returns 0. The header bytes are written into the `arena` (if non-null) or a small internal buffer; the payload iovec points directly at `frame.payload` (zero-copy).

### 2.3 `LineCodec` — line-delimited, zero-copy framing

**What it is.** Splits a stream on `\n` (LF) or `\r\n` (CRLF). The frame is `Line{ std::string_view data; }` — a **zero-copy view into the receive buffer** (delimiter stripped). Ideal for line protocols: Redis RESP, SMTP, POP3, IMAP, HTTP/1.x header lines.

```cpp
#include <qbuem/codec/line_codec.hpp>
using namespace qbuem::codec;

LineCodec codec(/*crlf=*/true);                    // true => \r\n, false => \n only
Line line;
BufferView view{data.data(), data.size()};
while (codec.decode(view, line) == DecodeStatus::Complete) {
    process(line.data);                            // string_view into `data` — DO NOT outlive `data`
}
// decode never returns Error; Incomplete means "no delimiter yet"
```

**Gotchas / constraints.**
- **`Line::data` aliases the input buffer.** If that buffer is freed or overwritten (e.g. the next `recv` into the same buffer), the view dangles. Copy out before reusing the buffer.
- Stateless: `reset()` is a no-op (provided for interface compatibility) and the codec never returns `Error`.
- `encode()` produces 2 iovecs (content + a static delimiter literal) — no allocation; `arena` is unused.

### 2.4 `Http1Codec` — HTTP/1.1 request framing

**What it is.** An `IFrameCodec<Request>` adapter around the SIMD `HttpParser` (`<qbuem/http/parser.hpp>`). `decode()` incrementally parses an HTTP/1.1 request into an `http::Request`. It is **server-side**: `encode()` always returns 0 (servers serialize *responses* via `http::Response::serialize()`, not requests).

```cpp
#include <qbuem/codec/http1_codec.hpp>
using namespace qbuem::codec;

Http1Codec codec;
Request req;
BufferView buf{recv.data(), recv.size()};
switch (codec.decode(buf, req)) {
  case DecodeStatus::Complete:
    handle_request(req);
    codec.reset();                 // keep-alive: prepare for the next request
    break;
  case DecodeStatus::Incomplete:
    if (codec.headers_complete()) { /* e.g. send 100-continue */ }
    break;
  case DecodeStatus::Error:
    int code = codec.error_status();   // 400 (malformed) or 413 (too large)
    /* send the corresponding response, then close */
    break;
}
```

**Gotchas / constraints.**
- `headers_complete()` lets you implement `Expect: 100-continue` (respond once headers are parsed, before the body).
- `error_status()` distinguishes **400** (bad syntax) from **413** (payload too large) so you can send the right response.
- `reset()` recreates the parser; call it between keep-alive requests and after errors.

> **Codec selection cheat-sheet:** length-prefix → binary RPC/messaging where you control both ends; line → text line protocols (RESP/SMTP/…); http1 → HTTP/1.1 servers. For higher-level HTTP handling (routing, responses) use the `App`/`Router` in `<qbuem/http/*>` directly rather than wiring `Http1Codec` by hand.

---

## Part 3 — Middleware (`<qbuem/middleware/*>`)

### The middleware interface

Middleware plugs into the HTTP `App`/`Router`. Two relevant aliases (from `<qbuem/http/router.hpp>`):

```cpp
using Handler    = std::function<void(const Request&, Response&)>;
using Middleware = std::function<bool(const Request&, Response&)>;   // return false => HALT chain
```

A `Middleware` returns **`true` to continue** the chain (run the next middleware / the route handler) or **`false` to halt** (the response it set is sent as-is). You register them with `app.use(...)`. Every factory below returns a `Middleware`. The runnable showcase is `examples/11-advanced-apps/middleware/` (and `security_middleware/`).

```cpp
#include <qbuem/qbuem_stack.hpp>
#include <qbuem/middleware/cors.hpp>
#include <qbuem/middleware/rate_limit.hpp>
#include <qbuem/middleware/request_id.hpp>
#include <qbuem/middleware/security.hpp>
#include <qbuem/middleware/token_auth.hpp>
using namespace qbuem;
using namespace qbuem::middleware;

App app(2);                                          // (worker count)
app.use(cors(CorsConfig{ .allow_origin = "https://example.com",
                         .allow_credentials = true, .max_age = 3600 }));
app.use(rate_limit(RateLimitConfig{ .rate_per_sec = 100.0, .burst = 20.0 }));
app.use(request_id("X-Request-ID"));
app.use(hsts(31'536'000, /*include_subdomains=*/true));
app.use(bearer_auth(verifier));                      // see token_auth below

app.get("/protected", [](const Request& req, Response& res) {
    auto sub = req.header("X-Auth-Sub");             // injected by bearer_auth
    res.status(200).header("Content-Type","application/json").body(/* ... */);
});
```

> **Order matters.** Middleware runs in registration order, *before* the route handler. Put `cors`/`rate_limit`/security early; put `bearer_auth` before the routes it protects. The chain stops at the first middleware that returns `false`.

### Built-in middleware catalog

| Factory | Header | Returns `false` (halts) when | Notes |
|---------|--------|------------------------------|-------|
| `cors(CorsConfig)` | `cors.hpp` | OPTIONS preflight (sends 204) | Static origin or dynamic `allow_origins` whitelist; sets `Vary: Origin` on whitelist match |
| `rate_limit(RateLimitConfig)` | `rate_limit.hpp` | bucket empty (sends 429 + `Retry-After`) | Per-thread token-bucket, **zero lock contention**; LRU-capped key map |
| `request_id(header)` | `request_id.hpp` | never | Echoes inbound ID or generates a CSPRNG UUID v4 |
| `require_content_type(ct, methods)` / `require_json()` | `content_type.hpp` | Content-Type mismatch (sends 415) | Only checks the listed methods (default POST/PUT/PATCH) |
| `hsts`, `csp`, `x_frame_options`, `x_content_type_options`, `referrer_policy`, `permissions_policy`, `secure_headers(SecureHeadersConfig)` | `security.hpp` | never | Header-injecting; `secure_headers` bundles them all |
| `bearer_auth(verifier[, opts])` | `token_auth.hpp` | missing/invalid Bearer token (sends 401) | Injects `ITokenVerifier`; forwards claims as `X-Auth-*` |
| `compress(IBodyEncoder&)` / `compress_response(...)` | `body_encoder.hpp` | n/a (post-processing) | Injects `IBodyEncoder`; see notes below |

### 3.1 `cors` — Cross-Origin Resource Sharing

```cpp
app.use(cors());                                     // allow-all "*" (dev / public API)
app.use(cors(CorsConfig{
    .allow_origin = "https://example.com",
    .allow_methods = "GET, POST, PUT, DELETE",
    .allow_headers = "Content-Type, Authorization",
    .allow_credentials = true,                       // requires a concrete (non-"*") origin
    .max_age = 3600,
}));
// Dynamic whitelist: reflect Origin only if listed; sets Vary: Origin
CorsConfig cfg; cfg.allow_origins = {"https://app.example.com","https://admin.example.com"};
app.use(cors(cfg));
```

**Gotchas.** Preflight `OPTIONS` is auto-answered with 204 and **halts the chain** (returns `false`). With a non-empty `allow_origins`, an origin not in the set gets *no* CORS headers and the chain continues (`true`). `allow_credentials = true` must pair with a concrete origin, never `"*"`.

### 3.2 `rate_limit` — token-bucket limiter

```cpp
app.use(rate_limit(RateLimitConfig{
    .rate_per_sec = 50.0,        // refill rate
    .max_keys     = 10'000,      // LRU cap on distinct keys (0 = unbounded)
    .burst        = 20.0,        // bucket capacity / first-burst allowance
}));
```

**Gotchas / constraints.**
- State is a **`thread_local` map per reactor thread** — zero lock contention, but the *effective* global limit is ≈ `rate_per_sec × worker_count`. Size accordingly.
- Default key = first IP of `X-Forwarded-For`, then `X-Real-IP`, then a shared `"__global__"` bucket. Keys are capped at 256 bytes (DoS guard). Override with `key_fn`.
- On limit: sends 429 + `X-RateLimit-Limit` / `X-RateLimit-Remaining` / `Retry-After` and returns `false`.
- `per_key_override` lets you raise/lower rate+burst per key (e.g. whitelist an internal IP).

### 3.3 `request_id` — correlation IDs

```cpp
app.use(request_id());                 // default header "X-Request-ID"
app.use(request_id("X-Trace-ID"));     // custom header
```

Echoes an inbound ID (set by a reverse proxy) or mints a fresh UUID v4. The UUID is generated from the in-tree **CSPRNG** (`crypto::random_fill`, backed by `getrandom`/`arc4random`), not Mersenne Twister — so IDs are unforgeable and safe to use as correlation/idempotency tokens. Always returns `true`.

### 3.4 `content_type` — media-type enforcement

```cpp
app.use(require_json());                                          // POST/PUT/PATCH must be application/json
app.use(require_content_type("multipart/form-data", {Method::Post}));   // only POST
```

Only enforces the listed methods (others pass through). On mismatch: 415 Unsupported Media Type + `false`. Matching is a substring `find`, so `application/json; charset=utf-8` satisfies `require_json()`.

### 3.5 `security` — security-header bundle

```cpp
app.use(secure_headers());                            // one call → the standard bundle
// or individually:
app.use(hsts(31'536'000, /*subdomains=*/true, /*preload=*/false));
app.use(csp("default-src 'self'; img-src *"));
app.use(x_frame_options("SAMEORIGIN"));
app.use(x_content_type_options());                    // nosniff
app.use(referrer_policy());                           // strict-origin-when-cross-origin
app.use(permissions_policy("camera=(), microphone=(), geolocation=()"));
```

`secure_headers(SecureHeadersConfig)` applies HSTS (configurable), CSP, X-Frame-Options, `X-Content-Type-Options: nosniff`, Referrer-Policy, and (if `perms_policy` non-empty) Permissions-Policy in a single middleware. All of these always return `true` (header-only). Only enable HSTS on HTTPS deployments.

### 3.6 `token_auth` — `ITokenVerifier` (zero-dependency auth)

**The pattern.** qbuem-stack does **not** implement JWT/PASETO/crypto itself (zero-dependency). You implement the abstract `ITokenVerifier` in your application — using OpenSSL, mbedTLS, an API-key table, etc. — and inject it into `bearer_auth`.

```cpp
#include <qbuem/middleware/token_auth.hpp>
using namespace qbuem::middleware;

class HS256Verifier : public ITokenVerifier {
public:
  explicit HS256Verifier(std::string secret) : secret_(std::move(secret)) {}
  // MUST be noexcept; return nullopt on ANY failure (bad sig, expired, malformed).
  std::optional<TokenClaims> verify(std::string_view token) noexcept override {
    // 1. split header.payload.sig  2. HMAC-SHA256  3. constant-time compare
    // 4. base64url-decode payload + parse claims  5. check exp/nbf
    TokenClaims c;
    c.subject = /* "sub" */; c.issuer = /* "iss" */;
    c.custom["role"] = /* ... */;
    return c;  // or std::nullopt
  }
private:
  std::string secret_;
};

// Preferred: shared_ptr overload (middleware co-owns the verifier — no dangling refs)
auto v = std::make_shared<HS256Verifier>("my-secret");
app.use(bearer_auth(v));

// Reference overload exists too, but the verifier MUST outlive the App:
HS256Verifier raw("s"); app.use(bearer_auth(raw));   // caller guarantees lifetime
```

**How claims reach the handler.** Because the `Request` is immutable inside middleware, verified claims are forwarded as **`X-Auth-*` response headers** (`claims_prefix`, default `"X-Auth-"`): `X-Auth-Sub`, `X-Auth-Iss`, `X-Auth-Aud`, and `X-Auth-<custom-key>`. The handler reads them back via `req.header("X-Auth-Sub")` / `res.get_header("X-Auth-Sub")`.

```cpp
BearerAuthOptions opts;
opts.claims_prefix = "X-Auth-";
opts.on_error = [](const Request&, Response& res, std::string_view reason) {
    res.status(401).header("WWW-Authenticate","Bearer").body(reason);
};
app.use(bearer_auth(v, opts));
```

**Gotchas / constraints.** `verify()` **must be `noexcept`** and thread-safe (called from multiple reactor threads — use stateless or `thread_local` state). Missing/invalid token → 401 + `WWW-Authenticate` and `false` (chain halts) unless you override `on_error`. `TokenClaims::exp`/`nbf` are `long` Unix timestamps (`-1` = absent). **Prefer the `shared_ptr` overload** to avoid dangling-reference bugs.

### 3.7 `body_encoder` — `IBodyEncoder` (zero-dependency compression)

**The pattern.** Same injection idiom as auth: qbuem-stack ships no compression library; you implement `IBodyEncoder` (zlib/brotli/zstd/lz4/…) and the framework calls it.

```cpp
#include <qbuem/middleware/body_encoder.hpp>
using namespace qbuem::middleware;

class GzipEncoder : public IBodyEncoder {
public:
  bool encode(std::string_view src, std::string& dst) noexcept override {
    /* zlib deflate src -> dst */ return true;     // false leaves body unchanged
  }
  std::string_view encoding_name() const noexcept override { return "gzip"; }  // Content-Encoding value
  std::string_view accept_token()  const noexcept override { return "gzip"; }  // Accept-Encoding match
};
```

**How to actually compress.** Note an important caveat baked into the header: qbuem's middleware chain runs **before** the handler, so the `compress(encoder)` middleware cannot post-process the body. The header provides the recommended path — call `compress_response(...)` from inside (or after) your handler once the body is set:

```cpp
GzipEncoder gzip;
app.get("/data", [&gzip](const Request& req, Response& res) {
    res.status(200).header("Content-Type","application/json").body(make_json());
    compress_response(gzip, req, res, /*min_size=*/256);   // gzips in place if eligible
});
```

`compress_response` only compresses when: no existing `Content-Encoding`, body `>= min_size` (default 256 B), a compressible `Content-Type` (text/*, application/json, xml, javascript, image/svg), and the request's `Accept-Encoding` contains `encoder.accept_token()`. On success it sets `Content-Encoding` + `Vary: Accept-Encoding` and moves the compressed body in. On `encode()` failure the original body is sent unchanged.

**Gotchas.** `encode()` must be `noexcept` and concurrency-safe. The bare `compress(encoder)` factory is effectively a no-op given the chain-ordering caveat — use `compress_response` from the handler.

### 3.8 `static_files` — MIME / ETag / file serving

Helpers used internally by `App::serve_static()`, also usable standalone to build custom file handlers:

```cpp
#include <qbuem/middleware/static_files.hpp>
using namespace qbuem::middleware;

app.get("/assets/app.js", [](const Request&, Response& res) {
    serve_file("/var/www/app.js", res);    // sets 200 + Content-Type + ETag + Last-Modified + body
});

std::string_view m = mime_type(".js");                 // "text/javascript; charset=utf-8"
std::string_view e = file_extension("/foo/bar.css");   // ".css"  ("" if none)
```

`serve_file` sets a weak ETag `W/"<size>-<mtime>"` from `stat(2)`, 404 for missing/non-regular files, 500 on read error. **On Linux** it records the path for a zero-copy `sendfile(2)` in the send loop (`res.sendfile_path(...)`); on other platforms (macOS aarch64) it reads the file into the response body. Only regular files are served.

### 3.9 `SseStream` — Server-Sent Events

**What it is.** A helper (namespace `qbuem`, in `middleware/sse.hpp`) that wraps a `Response` to push `text/event-stream` events over chunked transfer encoding. It sets the SSE headers on construction (`Content-Type: text/event-stream`, `Cache-Control: no-cache`, `X-Accel-Buffering: no`).

```cpp
#include <qbuem/middleware/sse.hpp>
using namespace qbuem;

app.get("/events", Handler([](const Request&, Response& res) {
    SseStream sse(res);
    sse.send("hello", "message");             // event: message\ndata: hello\n\n
    sse.send("42", "counter", "1");           // adds id: 1
    sse.send("reconnect", "msg", "2", 30000); // retry: 30000 ms
    sse.heartbeat();                          // ": ping\n\n" (keep-alive)
    sse.close();                              // terminates the chunked stream (also runs in dtor)
}));
```

`send(data, event = {}, id = {}, retry = -1)` returns `*this` for chaining and splits multi-line `data` across multiple `data:` fields. `close()` is idempotent and also called by the destructor.

**Gotchas / constraints.** In a synchronous handler, every `send()` buffers a chunk that is flushed when the handler returns (or on `close()`). For genuinely long-lived push streams use an **`AsyncHandler`** with `co_await sleep(...)` between events instead of looping inside one sync call — otherwise the response isn't sent until the handler returns. Send a `heartbeat()` roughly every ~15 s (browsers reconnect after ~30 s of silence).
