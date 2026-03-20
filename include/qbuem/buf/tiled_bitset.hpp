#pragma once

/**
 * @file qbuem/buf/tiled_bitset.hpp
 * @brief Infinite dynamic spatial bitset built from fixed-size GridBitset tiles.
 * @defgroup qbuem_buf_tiled TiledBitset
 * @ingroup qbuem_buf
 *
 * ## Overview
 * `TiledBitset<TileW, TileH, D>` provides an **unbounded** 2D (or 2.5D)
 * spatial bitset by dynamically allocating `GridBitset<TileW, TileH, D>`
 * tiles on demand.  World coordinates are signed `int64_t`, so the space
 * extends from −2^63 to 2^63−1 in both axes — effectively infinite for any
 * practical workload.
 *
 * ### Use cases
 * - **Open-world simulation** (game servers, physics engines)
 * - **Robotics / SLAM** — unbounded occupancy grid maps
 * - **GIS / cartography** — tiled spatial coverage layers
 * - **Semiconductor EDA** — layout DRC across large chips
 * - **RF / 5G** — multi-cell signal-presence maps
 * - **Warehouse / AMR** — dynamic floor-plan occupancy
 *
 * ### Architecture
 * - **Tile**      = one `GridBitset<TileW, TileH, D>` (~TileW × TileH × 8 bytes).
 * - **Tile key**  = packed `int32_t` pair (tx, ty) stored as `uint64_t`.
 * - **Tile map**  = `unordered_map<uint64_t, unique_ptr<Tile>>` protected by
 *                   a `shared_mutex` (shared for reads, exclusive for tile creation).
 * - **TLS cache** = 4-slot per-thread cache bypasses the mutex entirely on
 *                   repeated access to the same tile(s) — the common steady-state case.
 *
 * ### Thread Safety
 * - Reads/writes to existing tiles are lock-free (atomic `GridBitset` ops).
 * - Tile creation (`set` into an unloaded tile) acquires `unique_lock` once.
 * - After creation the TLS cache makes subsequent access mutex-free.
 * - `evict_empty_tiles()` and `reset_all()` require no concurrent writes.
 *
 * ### Performance (steady state, tile already loaded)
 * | Operation                      | Cost                                    |
 * |--------------------------------|-----------------------------------------|
 * | `set` / `clear` / `test`       | ~7–8 ns (TLS hit + atomic LOCK ORQ/ANDQ)|
 * | `any_in_box` (single tile)     | same as GridBitset (~1–3 ns for 8×8 SIMD)|
 * | `any_in_box` (N tiles crossed) | N × tile-lookup + SIMD row scan          |
 * | `any_in_radius` (r=20)         | ~20 ns (row-extent sqrt + SIMD scan)     |
 * | `raycast` (per step)           | ~8–10 ns (TLS hit + atomic load)         |
 *
 * ### Usage
 * ```cpp
 * // Unbounded occupancy grid, 32 vertical layers, 256×256 tiles
 * TiledBitset<256, 256, 32> space;
 *
 * space.set(-100'000, 50'000, 5);                   // negative coords work
 * bool hit = space.any_in_range(-100'000, 50'000, 0, 7);
 *
 * // Cross-tile box query (SIMD inside each tile)
 * uint32_t n = space.count_in_box(-300, -300, 300, 300, 0, 31);
 *
 * // Cross-tile raycast
 * auto r = space.raycast(-500, 0, 1, 0, 0, 31, 1000);
 * if (r) std::println("hit at ({}, {})", r->wx, r->wy);
 *
 * // Reclaim memory for abandoned regions
 * space.evict_empty_tiles();
 * ```
 */

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

#include <qbuem/buf/grid_bitset.hpp>

namespace qbuem {

/**
 * @brief Unbounded dynamic spatial bitset composed of fixed-size `GridBitset` tiles.
 *
 * @tparam TileW  Width  of each tile (columns). Recommended: power of two (e.g. 256).
 * @tparam TileH  Height of each tile (rows).    Recommended: power of two (e.g. 256).
 * @tparam D      Vertical layers per cell (≤ 64). Default: 64.
 */
template <uint32_t TileW, uint32_t TileH, uint32_t D = 64u>
class TiledBitset {
    static_assert(TileW > 0u, "TileW must be positive");
    static_assert(TileH > 0u, "TileH must be positive");
    static_assert(D >= 1u && D <= 64u, "Layer count D must be in [1, 64]");

public:
    using Tile       = GridBitset<TileW, TileH, D>;
    using WorldCoord = int64_t;
    using TileCoord  = int32_t;

    static constexpr uint32_t kTileW  = TileW;
    static constexpr uint32_t kTileH  = TileH;
    static constexpr uint32_t kLayers = D;

    // ── Ray hit result ────────────────────────────────────────────────────────

    /** @brief Result returned by `raycast()`. */
    struct RayHit {
        WorldCoord wx;          ///< World-coordinate hit column.
        WorldCoord wy;          ///< World-coordinate hit row.
        uint64_t   layer_mask;  ///< Full layer bitmap of the hit cell (ANDed with range).
        uint32_t   steps;       ///< DDA steps taken until the hit.
    };

    // ── Coordinate helpers (public — useful to callers) ───────────────────────

    /** @brief World column → tile column (floor division). */
    [[nodiscard]] static TileCoord tile_x(WorldCoord wx) noexcept {
        const int64_t tw = static_cast<int64_t>(TileW);
        return static_cast<TileCoord>(wx >= 0 ? wx / tw : (wx - tw + 1) / tw);
    }

    /** @brief World row → tile row (floor division). */
    [[nodiscard]] static TileCoord tile_y(WorldCoord wy) noexcept {
        const int64_t th = static_cast<int64_t>(TileH);
        return static_cast<TileCoord>(wy >= 0 ? wy / th : (wy - th + 1) / th);
    }

    /** @brief World column → local column within its tile [0, TileW). */
    [[nodiscard]] static uint32_t local_x(WorldCoord wx) noexcept {
        int64_t m = wx % static_cast<int64_t>(TileW);
        if (m < 0) m += TileW;
        return static_cast<uint32_t>(m);
    }

    /** @brief World row → local row within its tile [0, TileH). */
    [[nodiscard]] static uint32_t local_y(WorldCoord wy) noexcept {
        int64_t m = wy % static_cast<int64_t>(TileH);
        if (m < 0) m += TileH;
        return static_cast<uint32_t>(m);
    }

    /** @brief Tile + local → world column. */
    [[nodiscard]] static WorldCoord world_x(TileCoord tx, uint32_t lx) noexcept {
        return static_cast<WorldCoord>(tx) * TileW + lx;
    }

    /** @brief Tile + local → world row. */
    [[nodiscard]] static WorldCoord world_y(TileCoord ty, uint32_t ly) noexcept {
        return static_cast<WorldCoord>(ty) * TileH + ly;
    }

    // ── Construction ─────────────────────────────────────────────────────────

    TiledBitset()  = default;
    ~TiledBitset() = default;

    TiledBitset(const TiledBitset&)            = delete;
    TiledBitset& operator=(const TiledBitset&) = delete;

    // ── Write operations (auto-create tile if needed) ─────────────────────────

    /**
     * @brief Set layer `layer` at world position (wx, wy).
     *        Creates the tile on first write (one unique_lock; TLS-cached afterward).
     */
    void set(WorldCoord wx, WorldCoord wy, uint32_t layer) noexcept {
        get_or_create(tile_x(wx), tile_y(wy))
            ->set(local_x(wx), local_y(wy), layer);
    }

    /** @brief Clear layer `layer` at (wx, wy).  No-op if tile not yet loaded. */
    void clear(WorldCoord wx, WorldCoord wy, uint32_t layer) noexcept {
        if (auto* t = find(make_key(tile_x(wx), tile_y(wy))))
            t->clear(local_x(wx), local_y(wy), layer);
    }

    /** @brief OR `mask` into the layer column at (wx, wy).  Creates tile. */
    void set_column(WorldCoord wx, WorldCoord wy, uint64_t mask) noexcept {
        get_or_create(tile_x(wx), tile_y(wy))
            ->set_column(local_x(wx), local_y(wy), mask);
    }

    /**
     * @brief Atomically XOR-toggle layer `layer` at (wx, wy).  Creates tile.
     * @returns true if the bit is now SET.
     */
    bool toggle(WorldCoord wx, WorldCoord wy, uint32_t layer) noexcept {
        return get_or_create(tile_x(wx), tile_y(wy))
            ->toggle(local_x(wx), local_y(wy), layer);
    }

    /** @brief Clear ALL layers for cell (wx, wy).  No-op if tile not loaded. */
    void clear_all(WorldCoord wx, WorldCoord wy) noexcept {
        if (auto* t = find(make_key(tile_x(wx), tile_y(wy))))
            t->clear_all(local_x(wx), local_y(wy));
    }

    // ── Read operations (returns false / 0 for unloaded tiles) ───────────────

    /** @brief Test whether layer `layer` is set at (wx, wy). ~7 ns hot path. */
    [[nodiscard]] bool test(WorldCoord wx, WorldCoord wy,
                            uint32_t layer) const noexcept {
        auto* t = find(make_key(tile_x(wx), tile_y(wy)));
        return t && t->test(local_x(wx), local_y(wy), layer);
    }

    /** @brief True if any layer in [from, to] is set at (wx, wy). */
    [[nodiscard]] bool any_in_range(WorldCoord wx, WorldCoord wy,
                                    uint32_t from, uint32_t to) const noexcept {
        auto* t = find(make_key(tile_x(wx), tile_y(wy)));
        return t && t->any_in_range(local_x(wx), local_y(wy), from, to);
    }

    /** @brief Full 64-bit layer bitmap at (wx, wy).  Returns 0 if not loaded. */
    [[nodiscard]] uint64_t snapshot(WorldCoord wx, WorldCoord wy) const noexcept {
        auto* t = find(make_key(tile_x(wx), tile_y(wy)));
        return t ? t->snapshot(local_x(wx), local_y(wy)) : uint64_t{0};
    }

    // ── Box queries (may span multiple tiles) ─────────────────────────────────

    /**
     * @brief True if any occupied cell exists in [x1..x2]×[y1..y2] for
     *        layer range [from, to].  Efficiently skips unloaded tiles.
     */
    [[nodiscard]] bool any_in_box(WorldCoord x1, WorldCoord y1,
                                   WorldCoord x2, WorldCoord y2,
                                   uint32_t from, uint32_t to) const noexcept {
        assert(x1 <= x2 && y1 <= y2 && from <= to && to < D);
        const uint64_t  mask = detail::layer_range_mask(from, to);
        const TileCoord tx1  = tile_x(x1), tx2 = tile_x(x2);
        const TileCoord ty1  = tile_y(y1), ty2 = tile_y(y2);

        for (TileCoord ty = ty1; ty <= ty2; ++ty) {
            for (TileCoord tx = tx1; tx <= tx2; ++tx) {
                auto* t = find(make_key(tx, ty));
                if (!t) continue;
                const uint32_t lx1 = (tx == tx1) ? local_x(x1) : 0u;
                const uint32_t ly1 = (ty == ty1) ? local_y(y1) : 0u;
                const uint32_t lx2 = (tx == tx2) ? local_x(x2) : TileW - 1u;
                const uint32_t ly2 = (ty == ty2) ? local_y(y2) : TileH - 1u;
                for (uint32_t ry = ly1; ry <= ly2; ++ry) {
                    if (detail::scan_row_any(
                            t->row_ptr(ry) + lx1, lx2 - lx1 + 1u, mask))
                        return true;
                }
            }
        }
        return false;
    }

    /**
     * @brief Count occupied cells in [x1..x2]×[y1..y2] for layer range
     *        [from, to].  Unloaded tiles count as 0.
     */
    [[nodiscard]] uint32_t count_in_box(WorldCoord x1, WorldCoord y1,
                                         WorldCoord x2, WorldCoord y2,
                                         uint32_t from, uint32_t to) const noexcept {
        assert(x1 <= x2 && y1 <= y2 && from <= to && to < D);
        const uint64_t  mask = detail::layer_range_mask(from, to);
        const TileCoord tx1  = tile_x(x1), tx2 = tile_x(x2);
        const TileCoord ty1  = tile_y(y1), ty2 = tile_y(y2);

        uint32_t total = 0u;
        for (TileCoord ty = ty1; ty <= ty2; ++ty) {
            for (TileCoord tx = tx1; tx <= tx2; ++tx) {
                auto* t = find(make_key(tx, ty));
                if (!t) continue;
                const uint32_t lx1 = (tx == tx1) ? local_x(x1) : 0u;
                const uint32_t ly1 = (ty == ty1) ? local_y(y1) : 0u;
                const uint32_t lx2 = (tx == tx2) ? local_x(x2) : TileW - 1u;
                const uint32_t ly2 = (ty == ty2) ? local_y(y2) : TileH - 1u;
                for (uint32_t ry = ly1; ry <= ly2; ++ry)
                    total += detail::scan_row_count(
                        t->row_ptr(ry) + lx1, lx2 - lx1 + 1u, mask);
            }
        }
        return total;
    }

    // ── Radius queries (cross-tile, row-extent optimization) ──────────────────

    /**
     * @brief True if any occupied cell exists within Euclidean radius `r` of
     *        (cx, cy) for layer range [from, to].
     *
     * For each row in the bounding circle a single `sqrt` gives the exact
     * horizontal extent; `detail::scan_row_any` (SIMD) handles the scan,
     * splitting at tile boundaries transparently.
     */
    [[nodiscard]] bool any_in_radius(WorldCoord cx, WorldCoord cy,
                                      uint32_t r, uint32_t from,
                                      uint32_t to) const noexcept {
        assert(from <= to && to < D);
        const uint64_t mask = detail::layer_range_mask(from, to);
        const int64_t  r2   = static_cast<int64_t>(r) * r;

        for (int64_t wy = cy - r; wy <= cy + static_cast<int64_t>(r); ++wy) {
            const int64_t dy   = wy - cy;
            const int64_t half = static_cast<int64_t>(
                std::sqrt(static_cast<double>(r2 - dy * dy)));
            if (scan_world_row_any(cx - half, wy,
                    static_cast<uint32_t>(2 * half + 1), mask))
                return true;
        }
        return false;
    }

    /**
     * @brief Count occupied cells within Euclidean radius `r` of (cx, cy)
     *        for layer range [from, to].
     */
    [[nodiscard]] uint32_t count_in_radius(WorldCoord cx, WorldCoord cy,
                                            uint32_t r, uint32_t from,
                                            uint32_t to) const noexcept {
        assert(from <= to && to < D);
        const uint64_t mask = detail::layer_range_mask(from, to);
        const int64_t  r2   = static_cast<int64_t>(r) * r;

        uint32_t count = 0u;
        for (int64_t wy = cy - r; wy <= cy + static_cast<int64_t>(r); ++wy) {
            const int64_t dy   = wy - cy;
            const int64_t half = static_cast<int64_t>(
                std::sqrt(static_cast<double>(r2 - dy * dy)));
            count += scan_world_row_count(cx - half, wy,
                         static_cast<uint32_t>(2 * half + 1), mask);
        }
        return count;
    }

    /**
     * @brief Invoke `fn(wx, wy, layer_mask)` for every non-empty cell within
     *        Euclidean radius `r` of (cx, cy) in layer range [from, to].
     *
     * @tparam Fn Callable `void(WorldCoord wx, WorldCoord wy, uint64_t mask)`.
     */
    template <typename Fn>
    void for_each_set_in_radius(WorldCoord cx, WorldCoord cy,
                                uint32_t r, uint32_t from, uint32_t to,
                                Fn&& fn) const noexcept(noexcept(fn(0, 0, 0ull))) {
        assert(from <= to && to < D);
        const uint64_t range_mask = detail::layer_range_mask(from, to);
        const int64_t  r2         = static_cast<int64_t>(r) * r;

        for (int64_t wy = cy - r; wy <= cy + static_cast<int64_t>(r); ++wy) {
            const int64_t dy   = wy - cy;
            const int64_t half = static_cast<int64_t>(
                std::sqrt(static_cast<double>(r2 - dy * dy)));
            for (int64_t wx = cx - half; wx <= cx + half; ++wx) {
                const uint64_t snap = snapshot(wx, wy) & range_mask;
                if (snap) fn(wx, wy, snap);
            }
        }
    }

    // ── Iteration ─────────────────────────────────────────────────────────────

    /**
     * @brief Invoke `fn(wx, wy, layer_mask)` for every non-empty cell in
     *        [x1..x2]×[y1..y2] for layer range [from, to].
     *
     * @tparam Fn Callable `void(WorldCoord, WorldCoord, uint64_t)`.
     */
    template <typename Fn>
    void for_each_set_in_box(WorldCoord x1, WorldCoord y1,
                              WorldCoord x2, WorldCoord y2,
                              uint32_t from, uint32_t to,
                              Fn&& fn) const noexcept(noexcept(fn(0, 0, 0ull))) {
        assert(x1 <= x2 && y1 <= y2 && from <= to && to < D);
        const uint64_t  range_mask = detail::layer_range_mask(from, to);
        const TileCoord tx1 = tile_x(x1), tx2 = tile_x(x2);
        const TileCoord ty1 = tile_y(y1), ty2 = tile_y(y2);

        for (TileCoord ty = ty1; ty <= ty2; ++ty) {
            for (TileCoord tx = tx1; tx <= tx2; ++tx) {
                auto* t = find(make_key(tx, ty));
                if (!t) continue;
                const uint32_t lx1 = (tx == tx1) ? local_x(x1) : 0u;
                const uint32_t ly1 = (ty == ty1) ? local_y(y1) : 0u;
                const uint32_t lx2 = (tx == tx2) ? local_x(x2) : TileW - 1u;
                const uint32_t ly2 = (ty == ty2) ? local_y(y2) : TileH - 1u;
                for (uint32_t ry = ly1; ry <= ly2; ++ry) {
                    for (uint32_t rx = lx1; rx <= lx2; ++rx) {
                        const uint64_t snap =
                            t->row_ptr(ry)[rx].load(std::memory_order_relaxed)
                            & range_mask;
                        if (snap) fn(world_x(tx, rx), world_y(ty, ry), snap);
                    }
                }
            }
        }
    }

    /**
     * @brief Invoke `fn(tx, ty, tile_ref)` for every loaded tile.
     *
     * Useful for serialization, dirty-tile enumeration, and visualization.
     * @tparam Fn Callable `void(TileCoord tx, TileCoord ty, const Tile&)`.
     */
    template <typename Fn>
    void for_each_tile(Fn&& fn) const
            noexcept(noexcept(fn(0, 0, std::declval<const Tile&>()))) {
        std::shared_lock lock(tiles_mutex_);
        for (auto& [key, tile] : tiles_) {
            const auto [tx, ty] = unpack_key(key);
            fn(tx, ty, *tile);
        }
    }

    // ── Raycast (cross-tile DDA) ───────────────────────────────────────────────

    /**
     * @brief Walk a Bresenham DDA ray from world position (x0, y0) in direction
     *        (dx, dy) and return the first cell with any layer set in [from, to].
     *
     * Unloaded tiles are treated as empty.  Stops after `max_steps` DDA steps.
     *
     * @param x0, y0    World-coordinate ray origin.
     * @param dx, dy    Direction components (at least one non-zero).
     * @param from, to  Layer range to test.
     * @param max_steps Maximum DDA steps before giving up.
     * @returns `RayHit` on first hit, or `std::nullopt` if the path is clear.
     */
    [[nodiscard]] std::optional<RayHit>
    raycast(WorldCoord x0,  WorldCoord y0,
            int32_t    dx,  int32_t    dy,
            uint32_t from, uint32_t to,
            uint32_t max_steps) const noexcept {
        assert((dx != 0 || dy != 0) && from <= to && to < D);
        const uint64_t range_mask = detail::layer_range_mask(from, to);

        const int32_t sx  = (dx > 0) ? 1 : (dx < 0 ? -1 : 0);
        const int32_t sy  = (dy > 0) ? 1 : (dy < 0 ? -1 : 0);
        const int32_t adx = (dx < 0 ? -dx : dx);
        const int32_t ady = (dy < 0 ? -dy : dy);

        WorldCoord cx = x0, cy = y0;
        int32_t    err = adx - ady;

        for (uint32_t step = 0u; step < max_steps; ++step) {
            const uint64_t snap = snapshot(cx, cy) & range_mask;
            if (snap) return RayHit{cx, cy, snap, step};

            const int32_t e2 = 2 * err;
            if (e2 > -ady) { err -= ady; cx += sx; }
            if (e2 <  adx) { err += adx; cy += sy; }
        }
        return std::nullopt;
    }

    // ── Tile lifecycle ────────────────────────────────────────────────────────

    /** @brief Direct tile access by tile coordinates.  nullptr if not loaded. */
    [[nodiscard]] Tile* get_tile(TileCoord tx, TileCoord ty) const noexcept {
        return find(make_key(tx, ty));
    }

    /** @brief Ensure tile (tx, ty) is loaded (pre-allocates the tile). */
    Tile* prefetch_tile(TileCoord tx, TileCoord ty) noexcept {
        return get_or_create(tx, ty);
    }

    /** @brief True if tile (tx, ty) is currently loaded. */
    [[nodiscard]] bool has_tile(TileCoord tx, TileCoord ty) const noexcept {
        std::shared_lock lock(tiles_mutex_);
        return tiles_.count(make_key(tx, ty)) != 0u;
    }

    /**
     * @brief Free all tiles where every cell is zero (memory reclamation).
     *
     * Call periodically to release memory for abandoned regions.
     * NOT safe to call concurrently with writes.
     *
     * @returns Number of tiles freed.
     */
    size_t evict_empty_tiles() noexcept {
        std::unique_lock lock(tiles_mutex_);
        size_t freed = 0u;
        for (auto it = tiles_.begin(); it != tiles_.end(); ) {
            bool empty = true;
            it->second->for_each_set([&](uint32_t, uint32_t, uint64_t) noexcept {
                empty = false;
            });
            if (empty) { it = tiles_.erase(it); ++freed; }
            else        { ++it; }
        }
        if (freed > 0u)
            map_gen_.fetch_add(1u, std::memory_order_relaxed);
        return freed;
    }

    /** @brief Destroy all tiles and release all memory. */
    void reset_all() noexcept {
        std::unique_lock lock(tiles_mutex_);
        tiles_.clear();
        map_gen_.fetch_add(1u, std::memory_order_relaxed);
    }

    // ── Stats ─────────────────────────────────────────────────────────────────

    /** @brief Number of currently loaded tiles. */
    [[nodiscard]] size_t loaded_tile_count() const noexcept {
        std::shared_lock lock(tiles_mutex_);
        return tiles_.size();
    }

    /** @brief Approximate heap bytes used by all loaded tiles. */
    [[nodiscard]] size_t memory_bytes() const noexcept {
        std::shared_lock lock(tiles_mutex_);
        return tiles_.size() * sizeof(Tile);
    }

private:
    // ── Key packing ──────────────────────────────────────────────────────────

    [[nodiscard]] static uint64_t make_key(TileCoord tx, TileCoord ty) noexcept {
        return (static_cast<uint64_t>(static_cast<uint32_t>(tx)) << 32u)
             |  static_cast<uint64_t>(static_cast<uint32_t>(ty));
    }

    [[nodiscard]] static std::pair<TileCoord, TileCoord>
    unpack_key(uint64_t key) noexcept {
        return {static_cast<TileCoord>(static_cast<uint32_t>(key >> 32u)),
                static_cast<TileCoord>(static_cast<uint32_t>(key))};
    }

    // ── Tile map ──────────────────────────────────────────────────────────────

    struct KeyHash {
        size_t operator()(uint64_t k) const noexcept {
            // Murmur3 finalizer — good avalanche for packed (tx, ty) pairs.
            k ^= k >> 33u;
            k *= 0xff51afd7ed558ccdull;
            k ^= k >> 33u;
            k *= 0xc4ceb9fe1a85ec53ull;
            k ^= k >> 33u;
            return static_cast<size_t>(k);
        }
    };

    // Globally unique ID per TiledBitset instance.
    // Used in the TLS cache key so that two instances at the same address
    // (common with Google Test fixture reuse) never alias each other's cache
    // entries — preventing use-after-free of freed tile pointers.
    static inline std::atomic<uint64_t> s_instance_counter_{0u};
    uint64_t const                      instance_id_{
        s_instance_counter_.fetch_add(1u, std::memory_order_relaxed)};

    mutable std::shared_mutex tiles_mutex_;
    std::unordered_map<uint64_t, std::unique_ptr<Tile>, KeyHash> tiles_;
    std::atomic<uint32_t> map_gen_{0u}; // incremented on tile create/eviction

    // ── TLS 4-slot cache ──────────────────────────────────────────────────────
    // Bypasses shared_mutex on repeated access to the same ~4 tiles per thread.
    //
    // Correctness: if cache.gen == map_gen_ && cache.owner == this, ptr is valid.
    // A gen increment (tile created or evicted) causes re-lookup on next access.

    struct alignas(64) TLSEntry {
        uint64_t              instance_id{~uint64_t{0}}; // per-instance unique ID
        uint32_t              gen{~0u};
        uint64_t              key{~uint64_t{0}};
        Tile*                 ptr{nullptr};
    };

    [[nodiscard]] Tile* find(uint64_t key) const noexcept {
        thread_local TLSEntry cache[4];
        thread_local uint32_t next{0u};

        const uint32_t gen = map_gen_.load(std::memory_order_relaxed);

        // Hot path: linear scan of 4 cache entries.
        for (auto& e : cache) {
            if (e.instance_id == instance_id_ && e.gen == gen && e.key == key) [[likely]]
                return e.ptr;
        }

        // Cold path: acquire shared_lock and look up the map.
        Tile* ptr;
        {
            std::shared_lock lock(tiles_mutex_);
            auto it = tiles_.find(key);
            ptr = (it != tiles_.end()) ? it->second.get() : nullptr;
        }

        // Evict oldest slot (circular) and cache the result.
        auto& slot = cache[next & 3u];
        ++next;
        slot = {instance_id_, gen, key, ptr};
        return ptr;
    }

    // Get existing tile or create a new one (write path, unique_lock).
    [[nodiscard]] Tile* get_or_create(TileCoord tx, TileCoord ty) noexcept {
        const uint64_t key = make_key(tx, ty);

        // Fast path: tile already loaded (TLS cache hit).
        if (auto* t = find(key)) [[likely]] return t;

        // Slow path: create tile under exclusive lock (rare, once per tile).
        Tile* ptr;
        {
            std::unique_lock lock(tiles_mutex_);
            auto [it, inserted] = tiles_.emplace(key, nullptr);
            if (inserted) {
                it->second = std::make_unique<Tile>();
                map_gen_.fetch_add(1u, std::memory_order_relaxed);
            }
            ptr = it->second.get();
        }
        return ptr;
    }

    // ── Cross-tile row scan helpers ───────────────────────────────────────────

    // Scan world-row [wx_start, wx_start + len) for any (value & mask) != 0.
    // Splits at tile boundaries and calls detail::scan_row_any per segment.
    [[nodiscard]] bool scan_world_row_any(WorldCoord wx_start, WorldCoord wy,
                                          uint32_t len, uint64_t mask) const noexcept {
        const TileCoord ty = tile_y(wy);
        const uint32_t  ly = local_y(wy);
        WorldCoord wx      = wx_start;
        uint32_t remaining = len;

        while (remaining > 0u) {
            const TileCoord tx       = tile_x(wx);
            const uint32_t  lx       = local_x(wx);
            const uint32_t  avail    = TileW - lx; // cells until tile boundary
            const uint32_t  scan_len = avail < remaining ? avail : remaining;

            auto* t = find(make_key(tx, ty));
            if (t && detail::scan_row_any(t->row_ptr(ly) + lx, scan_len, mask))
                return true;

            wx        += static_cast<WorldCoord>(scan_len);
            remaining -= scan_len;
        }
        return false;
    }

    // Count non-zero cells in world-row [wx_start, wx_start + len).
    [[nodiscard]] uint32_t scan_world_row_count(WorldCoord wx_start, WorldCoord wy,
                                                 uint32_t len, uint64_t mask) const noexcept {
        const TileCoord ty = tile_y(wy);
        const uint32_t  ly = local_y(wy);
        WorldCoord wx      = wx_start;
        uint32_t remaining = len;
        uint32_t count     = 0u;

        while (remaining > 0u) {
            const TileCoord tx       = tile_x(wx);
            const uint32_t  lx       = local_x(wx);
            const uint32_t  avail    = TileW - lx;
            const uint32_t  scan_len = avail < remaining ? avail : remaining;

            auto* t = find(make_key(tx, ty));
            if (t) count += detail::scan_row_count(t->row_ptr(ly) + lx, scan_len, mask);

            wx        += static_cast<WorldCoord>(scan_len);
            remaining -= scan_len;
        }
        return count;
    }
};

} // namespace qbuem
