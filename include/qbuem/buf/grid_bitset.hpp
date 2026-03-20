#pragma once

/**
 * @file qbuem/buf/grid_bitset.hpp
 * @brief Spatial bitset for 2D / 2.5D / 3D grids with wait-free atomic reads.
 * @defgroup qbuem_buf_grid GridBitset
 * @ingroup qbuem_buf
 *
 * ## Overview
 * `GridBitset<W, H, D>` is a flat, cache-line-aware spatial bitset tuned for
 * nanosecond-range point queries, layer-range queries, and box queries.
 *
 * ### Dimension Modes
 * | D      | Mode  | Storage per (x,y)       | Max layers |
 * |--------|-------|--------------------------|------------|
 * | 1      | 2D    | 1 bit (packed uint64_t)  | –          |
 * | 2..64  | 2.5D  | 1 uint64_t (64 layers)   | D ≤ 64     |
 *
 * ### 2.5D "SIMD Verticality" (primary mode, D ≤ 64)
 * Each (x, y) cell stores up to 64 vertical layers in one `uint64_t`.
 * A layer-range query ("any obstacle between floor 2 and 5?") is a
 * single bitwise-AND + compare — approximately 1 ns.
 *
 * ```cpp
 * // Build a 256×256 grid with 16 vertical layers
 * GridBitset<256, 256, 16> grid;
 * grid.set(10, 20, 3);                           // set layer 3 at (10,20)
 * bool hit = grid.any_in_range(10, 20, 2, 5);   // is anything on floors 2-5?
 * uint64_t col = grid.snapshot(10, 20);          // full layer bitmap
 * ```
 *
 * ### 2D Mode (D == 1)
 * Uses `GridBitset2D<W, H>` directly.  Bits are tightly packed into
 * `uint64_t` words with Morton-code spatial layout (using BMI2 PDEP/PEXT
 * when available) so that nearby cells are also nearby in memory.
 *
 * ```cpp
 * GridBitset2D<64, 64> map;
 * map.set(3, 7);
 * bool occupied = map.test(3, 7);
 * ```
 *
 * ### Morton Code (cache-line blocking)
 * Super-blocks of 8×8 = 64 bits are stored with Morton order, so
 * "find-in-box" queries touch the minimum number of cache lines.
 * On x86 with BMI2 (`-mbmi2`), `_pdep_u32` interleaves in O(1).
 * A portable fallback is compiled otherwise.
 *
 * ### Game-Ready API Extensions
 * Both `GridBitset` and `GridBitset2D` expose a uniform set of APIs designed
 * for game and simulation workloads:
 *
 * | API | Description |
 * |-----|-------------|
 * | `toggle(x,y[,layer])` | XOR-flip one bit — doors, switches, destructibles |
 * | `lowest_layer / highest_layer` | Ground/ceiling detection (BSF/CLZ, ~1 ns) |
 * | `count_layers / count_all` | Population counts (POPCNT) |
 * | `any_in_column` | Broad-phase: any obstacle at (x,y)? |
 * | `for_each_set[_in_box]` | Iterate occupied cells with sparse-skip |
 * | `merge_from` | OR — union two grids (spawn, merge zones) |
 * | `intersect_with` | AND — shared visibility, overlap detection |
 * | `diff_from` | AND-NOT — remove destroyed obstacles |
 * | `clear_box` | Bulk-clear a rectangular region |
 * | `raycast / raycast_2d` | Bresenham DDA line-of-sight / bullet trace |
 *
 * ### Thread Safety
 * - Reads (`test`, `any_in_range`, `snapshot`, `any_in_box`, `for_each_set`,
 *   `raycast`, `raycast_2d`, `lowest_layer`, `highest_layer`, `count_*`) are wait-free.
 * - Writes (`set`, `clear`, `toggle`, `merge_from`, `diff_from`,
 *   `intersect_with`, `clear_box`) use `fetch_or`/`fetch_and`/`fetch_xor`
 *   (`memory_order_release` / `memory_order_acquire`).
 * - Multiple writers to different cells are fully safe.
 * - Concurrent write + read of the **same cell** is safe (atomic).
 *
 * @code
 * // 2.5D player visibility grid (256×256, 64 floors)
 * GridBitset<256, 256, 64> visibility;
 * visibility.set(px, py, floor);
 * bool can_see = visibility.any_in_range(px, py, 0, 10);
 *
 * // 2D obstacle map
 * GridBitset2D<128, 128> obstacles;
 * obstacles.set(x, y);
 * bool blocked = obstacles.test(x, y);
 * @endcode
 */

#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

// ── SIMD / BMI2 detection ─────────────────────────────────────────────────────
#if defined(__AVX512F__) && defined(__AVX512VL__)
#  include <immintrin.h>
#  define QBUEM_GRID_SIMD_AVX512 1
#elif defined(__AVX2__)
#  include <immintrin.h>
#  define QBUEM_GRID_SIMD_AVX2   1
#elif defined(__ARM_NEON)
#  include <arm_neon.h>
#  define QBUEM_GRID_SIMD_NEON   1
#endif

#if defined(__BMI2__)
#  ifndef QBUEM_GRID_SIMD_AVX512
#  ifndef QBUEM_GRID_SIMD_AVX2
#    include <immintrin.h>   // BMI2 only — immintrin not yet included above
#  endif
#  endif
#  define QBUEM_HAS_BMI2 1
#else
#  define QBUEM_HAS_BMI2 0
#endif

namespace qbuem {

// ── Morton code utilities ──────────────────────────────────────────────────────

namespace detail {

/**
 * @brief Interleave bits of x (even bits) and y (odd bits) — 2D Morton code.
 *
 * On BMI2 hardware: uses PDEP (one instruction, O(1)).
 * Portable fallback: bit-spreading via successive OR+shift.
 *
 * @param x Coordinate; upper 16 bits are ignored.
 * @param y Coordinate; upper 16 bits are ignored.
 * @returns 32-bit Morton code Z(x, y).
 */
[[nodiscard]] inline uint32_t morton2d(uint32_t x, uint32_t y) noexcept {
#if QBUEM_HAS_BMI2
    // PDEP: scatter bits of x into even positions, y into odd positions.
    return _pdep_u32(x, 0x55555555u) | _pdep_u32(y, 0xAAAAAAAAu);
#else
    // Portable: widen each 16-bit coordinate to 32 bits by interleaving zeros.
    auto spread16 = [](uint32_t v) noexcept -> uint32_t {
        v &= 0x0000'ffffu;
        v = (v | (v << 8u))  & 0x00ff'00ffu;
        v = (v | (v << 4u))  & 0x0f0f'0f0fu;
        v = (v | (v << 2u))  & 0x3333'3333u;
        v = (v | (v << 1u))  & 0x5555'5555u;
        return v;
    };
    return spread16(x) | (spread16(y) << 1u);
#endif
}

/**
 * @brief Extract x (even bits) and y (odd bits) from a 2D Morton code.
 *
 * On BMI2: uses PEXT.  Portable fallback otherwise.
 *
 * @param m Morton code.
 * @param x Out: x coordinate.
 * @param y Out: y coordinate.
 */
inline void morton2d_decode(uint32_t m, uint32_t& x, uint32_t& y) noexcept {
#if QBUEM_HAS_BMI2
    x = _pext_u32(m, 0x55555555u);
    y = _pext_u32(m, 0xAAAAAAAAu);
#else
    auto compact = [](uint32_t v) noexcept -> uint32_t {
        v &= 0x5555'5555u;
        v = (v | (v >> 1u))  & 0x3333'3333u;
        v = (v | (v >> 2u))  & 0x0f0f'0f0fu;
        v = (v | (v >> 4u))  & 0x00ff'00ffu;
        v = (v | (v >> 8u))  & 0x0000'ffffu;
        return v;
    };
    x = compact(m);
    y = compact(m >> 1u);
#endif
}

/** @brief Build a contiguous bitmask covering bits [from, to] inclusive. */
[[nodiscard]] constexpr uint64_t layer_range_mask(uint32_t from, uint32_t to) noexcept {
    assert(from <= to && to < 64u);
    if (to >= 63u) return ~uint64_t{0} << from;
    return ((uint64_t{1} << (to - from + 1u)) - 1u) << from;
}

} // namespace detail

// ── GridBitset — 2.5D spatial bitset (primary implementation) ─────────────────

/**
 * @brief Wait-free spatial bitset for 2.5D grids.
 *
 * Each (x, y) cell stores up to D vertical layers (D ≤ 64) in a single
 * `std::atomic<uint64_t>`, enabling ~1 ns layer-range queries via a
 * single bitwise-AND.
 *
 * Template requirements:
 * - `W > 0`, `H > 0`, `D` in [1, 64].
 * - Total cells (W × H) must fit in `size_t`.
 *
 * @tparam W  Grid width (x dimension, columns).
 * @tparam H  Grid height (y dimension, rows).
 * @tparam D  Number of vertical layers (≤ 64). Default: 64.
 */
template <uint32_t W, uint32_t H, uint32_t D = 64u>
class GridBitset {
    static_assert(W > 0u, "Grid width must be positive");
    static_assert(H > 0u, "Grid height must be positive");
    static_assert(D >= 1u && D <= 64u, "Layer count D must be in [1, 64]");

    // Effective bit mask for the D active layers (prevents stray bit writes).
    static constexpr uint64_t kLayerMask =
        (D == 64u) ? ~uint64_t{0} : ((uint64_t{1} << D) - 1u);

public:
    static constexpr uint32_t kWidth  = W;
    static constexpr uint32_t kHeight = H;
    static constexpr uint32_t kLayers = D;
    static constexpr size_t   kCells  = static_cast<size_t>(W) * H;

    // ── Construction ─────────────────────────────────────────────────────────

    /** @brief Zero-initialize all cells (all layers unoccupied). */
    GridBitset() noexcept {
        for (auto& c : cells_)
            c.store(uint64_t{0}, std::memory_order_relaxed);
    }

    GridBitset(const GridBitset&)            = delete;
    GridBitset& operator=(const GridBitset&) = delete;

    // ── Write operations ─────────────────────────────────────────────────────

    /**
     * @brief Mark a single layer as occupied.
     * @param x     Column index [0, W).
     * @param y     Row index    [0, H).
     * @param layer Vertical layer [0, D).
     */
    void set(uint32_t x, uint32_t y, uint32_t layer) noexcept {
        assert(x < W && y < H && layer < D);
        cells_[flat(x, y)].fetch_or(uint64_t{1} << layer,
                                    std::memory_order_release);
    }

    /**
     * @brief Clear a single layer (mark as unoccupied).
     * @param x     Column index [0, W).
     * @param y     Row index    [0, H).
     * @param layer Vertical layer [0, D).
     */
    void clear(uint32_t x, uint32_t y, uint32_t layer) noexcept {
        assert(x < W && y < H && layer < D);
        cells_[flat(x, y)].fetch_and(~(uint64_t{1} << layer),
                                     std::memory_order_release);
    }

    /**
     * @brief OR a precomputed layer bitmask into a cell.
     *
     * Useful for batch-setting multiple layers at once.
     * @param x    Column index [0, W).
     * @param y    Row index    [0, H).
     * @param mask Bitmask; bits beyond kLayers are silently ignored.
     */
    void set_column(uint32_t x, uint32_t y, uint64_t mask) noexcept {
        assert(x < W && y < H);
        cells_[flat(x, y)].fetch_or(mask & kLayerMask,
                                    std::memory_order_release);
    }

    /**
     * @brief Atomically replace the entire layer column for a cell.
     * @param x    Column index [0, W).
     * @param y    Row index    [0, H).
     * @param mask New bitmask; bits beyond kLayers are silently cleared.
     */
    void store_column(uint32_t x, uint32_t y, uint64_t mask) noexcept {
        assert(x < W && y < H);
        cells_[flat(x, y)].store(mask & kLayerMask,
                                 std::memory_order_release);
    }

    /** @brief Clear all layers for a cell in one atomic write. */
    void clear_all(uint32_t x, uint32_t y) noexcept {
        assert(x < W && y < H);
        cells_[flat(x, y)].store(uint64_t{0}, std::memory_order_release);
    }

    /** @brief Reset the entire grid to zero. Not atomic w.r.t. concurrent reads. */
    void reset() noexcept {
        for (auto& c : cells_)
            c.store(uint64_t{0}, std::memory_order_relaxed);
    }

    // ── Read operations (wait-free) ───────────────────────────────────────────

    /**
     * @brief Test whether a single layer is occupied. ~2 ns.
     * @returns true if the bit at (x, y, layer) is set.
     */
    [[nodiscard]] bool test(uint32_t x, uint32_t y, uint32_t layer) const noexcept {
        assert(x < W && y < H && layer < D);
        return (cells_[flat(x, y)].load(std::memory_order_acquire) >> layer) & 1u;
    }

    /**
     * @brief Query whether any layer in [from, to] is occupied. ~1 ns.
     *
     * Equivalent to the "SIMD Verticality" query from the design document:
     * `(bitmap[idx] & mask_from_to) != 0`
     *
     * @param x    Column index [0, W).
     * @param y    Row index    [0, H).
     * @param from First layer (inclusive).
     * @param to   Last  layer (inclusive, ≤ D-1).
     * @returns true if any bit in the range is set.
     */
    [[nodiscard]] bool any_in_range(uint32_t x, uint32_t y,
                                    uint32_t from, uint32_t to) const noexcept {
        assert(x < W && y < H && from <= to && to < D);
        const uint64_t mask = detail::layer_range_mask(from, to);
        return (cells_[flat(x, y)].load(std::memory_order_acquire) & mask) != 0u;
    }

    /**
     * @brief Read the full 64-bit layer bitmap for one cell (wait-free snapshot).
     * @returns Bitmask where bit k is set iff layer k is occupied.
     */
    [[nodiscard]] uint64_t snapshot(uint32_t x, uint32_t y) const noexcept {
        assert(x < W && y < H);
        return cells_[flat(x, y)].load(std::memory_order_acquire);
    }

    /**
     * @brief Check whether any cell in an axis-aligned bounding box is occupied
     *        in the given layer range.
     *
     * Iterates the (x1..x2) × (y1..y2) rectangle and returns true on first hit.
     * With AVX-512 support, the per-cell check reduces to `VPAND + VPTST`.
     *
     * @param x1, y1  Top-left corner (inclusive).
     * @param x2, y2  Bottom-right corner (inclusive).
     * @param from    First layer (inclusive).
     * @param to      Last  layer (inclusive).
     * @returns true if any occupied cell exists in the box.
     */
    /**
     * @brief Check whether any cell in an axis-aligned bounding box is occupied
     *        in the given layer range.
     *
     * Uses AVX-512 (8 cells/load), AVX2 (4 cells/load), or NEON (2 cells/load)
     * SIMD paths when available.  Falls back to scalar on other platforms.
     */
    [[nodiscard]] bool any_in_box(uint32_t x1, uint32_t y1,
                                  uint32_t x2, uint32_t y2,
                                  uint32_t from, uint32_t to) const noexcept {
        assert(x1 <= x2 && x2 < W && y1 <= y2 && y2 < H);
        assert(from <= to && to < D);
        const uint64_t mask  = detail::layer_range_mask(from, to);
        const uint32_t box_w = x2 - x1 + 1u;

#if defined(QBUEM_GRID_SIMD_AVX512)
        // AVX-512: load 8 × uint64 per instruction.
        const __m512i vmask = _mm512_set1_epi64(static_cast<long long>(mask));
        for (uint32_t ry = y1; ry <= y2; ++ry) {
            const auto* row = cells_ + static_cast<size_t>(ry) * W + x1;
            uint32_t rx = 0u;
            for (; rx + 8u <= box_w; rx += 8u) {
                __m512i v = _mm512_loadu_si512(
                    reinterpret_cast<const __m512i*>(row + rx));
                if (_mm512_test_epi64_mask(v, vmask)) return true;
            }
            for (; rx < box_w; ++rx)
                if (row[rx].load(std::memory_order_relaxed) & mask) return true;
        }
#elif defined(QBUEM_GRID_SIMD_AVX2)
        // AVX2: load 4 × uint64 per instruction.
        const __m256i vmask = _mm256_set1_epi64x(static_cast<long long>(mask));
        for (uint32_t ry = y1; ry <= y2; ++ry) {
            const auto* row = cells_ + static_cast<size_t>(ry) * W + x1;
            uint32_t rx = 0u;
            for (; rx + 4u <= box_w; rx += 4u) {
                __m256i v = _mm256_loadu_si256(
                    reinterpret_cast<const __m256i*>(row + rx));
                __m256i r = _mm256_and_si256(v, vmask);
                if (!_mm256_testz_si256(r, r)) return true;
            }
            for (; rx < box_w; ++rx)
                if (row[rx].load(std::memory_order_relaxed) & mask) return true;
        }
#elif defined(QBUEM_GRID_SIMD_NEON)
        // NEON (AArch64): load 2 × uint64 per instruction.
        const uint64x2_t vmask = vdupq_n_u64(mask);
        for (uint32_t ry = y1; ry <= y2; ++ry) {
            const auto* row = cells_ + static_cast<size_t>(ry) * W + x1;
            uint32_t rx = 0u;
            for (; rx + 2u <= box_w; rx += 2u) {
                uint64x2_t v = vld1q_u64(
                    reinterpret_cast<const uint64_t*>(row + rx));
                uint64x2_t r = vandq_u64(v, vmask);
                if (vmaxvq_u64(r)) return true;
            }
            for (; rx < box_w; ++rx)
                if (row[rx].load(std::memory_order_relaxed) & mask) return true;
        }
#else
        // Scalar fallback.
        for (uint32_t ry = y1; ry <= y2; ++ry) {
            for (uint32_t rx = x1; rx <= x2; ++rx) {
                if (cells_[flat(rx, ry)].load(std::memory_order_relaxed) & mask)
                    return true;
            }
        }
#endif
        return false;
    }

    /**
     * @brief Count occupied cells in a box for a given layer range.
     *
     * @returns Number of cells in the box where at least one layer in
     *          [from, to] is set.
     */
    [[nodiscard]] uint32_t count_in_box(uint32_t x1, uint32_t y1,
                                        uint32_t x2, uint32_t y2,
                                        uint32_t from, uint32_t to) const noexcept {
        assert(x1 <= x2 && x2 < W && y1 <= y2 && y2 < H);
        assert(from <= to && to < D);
        const uint64_t mask = detail::layer_range_mask(from, to);
        uint32_t count = 0u;
        for (uint32_t ry = y1; ry <= y2; ++ry) {
            for (uint32_t rx = x1; rx <= x2; ++rx) {
                if (cells_[flat(rx, ry)].load(std::memory_order_relaxed) & mask)
                    ++count;
            }
        }
        return count;
    }

    // ── Atomic bit flip ───────────────────────────────────────────────────────

    /**
     * @brief Atomically toggle a single layer bit (XOR).
     *
     * Useful for toggling doors, switches, destructible terrain, etc.
     * @param x     Column index [0, W).
     * @param y     Row index    [0, H).
     * @param layer Vertical layer [0, D).
     * @returns true if the bit is now set (after the toggle).
     */
    bool toggle(uint32_t x, uint32_t y, uint32_t layer) noexcept {
        assert(x < W && y < H && layer < D);
        const uint64_t bit  = uint64_t{1} << layer;
        const uint64_t prev = cells_[flat(x, y)].fetch_xor(bit,
                                                            std::memory_order_acq_rel);
        return (prev & bit) == 0u; // true = bit is now SET
    }

    // ── Column-level queries ──────────────────────────────────────────────────

    /**
     * @brief Test whether any layer is occupied at (x, y). ~1 ns.
     *
     * Equivalent to `snapshot(x, y) != 0`. Useful for broad-phase checks
     * before a more specific `any_in_range`.
     */
    [[nodiscard]] bool any_in_column(uint32_t x, uint32_t y) const noexcept {
        assert(x < W && y < H);
        return cells_[flat(x, y)].load(std::memory_order_acquire) != 0u;
    }

    /**
     * @brief Return the index of the lowest (ground-nearest) occupied layer.
     *
     * Returns D if no layer is set (no floor detected).
     * Uses `std::countr_zero` — single BSF/TZCNT instruction on x86/AArch64.
     *
     * @param x Column [0, W). @param y Row [0, H).
     * @returns Layer index in [0, D), or D if the column is empty.
     */
    [[nodiscard]] uint32_t lowest_layer(uint32_t x, uint32_t y) const noexcept {
        assert(x < W && y < H);
        const uint64_t snap = cells_[flat(x, y)].load(std::memory_order_acquire)
                              & kLayerMask;
        return snap ? static_cast<uint32_t>(std::countr_zero(snap)) : D;
    }

    /**
     * @brief Return the index of the highest (ceiling-nearest) occupied layer.
     *
     * Returns D if no layer is set.
     * Uses `std::countl_zero` — single BSR/CLZ instruction.
     *
     * @param x Column [0, W). @param y Row [0, H).
     * @returns Layer index in [0, D), or D if the column is empty.
     */
    [[nodiscard]] uint32_t highest_layer(uint32_t x, uint32_t y) const noexcept {
        assert(x < W && y < H);
        const uint64_t snap = cells_[flat(x, y)].load(std::memory_order_acquire)
                              & kLayerMask;
        return snap ? static_cast<uint32_t>(63u - std::countl_zero(snap)) : D;
    }

    /**
     * @brief Count the number of occupied layers in a single column. ~1 ns.
     *
     * Uses POPCNT — one instruction on modern CPUs.
     *
     * @param x Column [0, W). @param y Row [0, H).
     */
    [[nodiscard]] uint32_t count_layers(uint32_t x, uint32_t y) const noexcept {
        assert(x < W && y < H);
        return static_cast<uint32_t>(
            std::popcount(cells_[flat(x, y)].load(std::memory_order_acquire)
                          & kLayerMask));
    }

    // ── Grid-wide aggregates ──────────────────────────────────────────────────

    /**
     * @brief Total occupied layer-slots across the entire grid.
     *
     * Sums `popcount` over all W×H cells.  O(W×H).
     */
    [[nodiscard]] uint32_t count_total() const noexcept {
        uint32_t total = 0u;
        for (const auto& c : cells_)
            total += static_cast<uint32_t>(
                std::popcount(c.load(std::memory_order_relaxed) & kLayerMask));
        return total;
    }

    // ── Bulk-clear ────────────────────────────────────────────────────────────

    /**
     * @brief Clear all layers in [from, to] for every cell in a box.
     *
     * Useful for clearing explosion zones, opening passages, resetting rooms.
     *
     * @param x1,y1 Top-left corner (inclusive).
     * @param x2,y2 Bottom-right corner (inclusive).
     * @param from  First layer (inclusive).
     * @param to    Last  layer (inclusive).
     */
    void clear_box(uint32_t x1, uint32_t y1,
                   uint32_t x2, uint32_t y2,
                   uint32_t from, uint32_t to) noexcept {
        assert(x1 <= x2 && x2 < W && y1 <= y2 && y2 < H);
        assert(from <= to && to < D);
        const uint64_t mask = ~detail::layer_range_mask(from, to);
        for (uint32_t ry = y1; ry <= y2; ++ry)
            for (uint32_t rx = x1; rx <= x2; ++rx)
                cells_[flat(rx, ry)].fetch_and(mask, std::memory_order_release);
    }

    // ── Iteration ─────────────────────────────────────────────────────────────

    /**
     * @brief Invoke `fn(x, y, layer_mask)` for every non-empty cell.
     *
     * `layer_mask` is the full 64-bit layer bitmap for that cell.
     * Iteration order: row-major (y then x).
     *
     * Callback signature: `void fn(uint32_t x, uint32_t y, uint64_t mask)`.
     *
     * @tparam Fn Callable matching `void(uint32_t, uint32_t, uint64_t)`.
     */
    template <typename Fn>
    void for_each_set(Fn&& fn) const noexcept(noexcept(fn(0u, 0u, 0ull))) {
        for (uint32_t ry = 0u; ry < H; ++ry) {
            for (uint32_t rx = 0u; rx < W; ++rx) {
                const uint64_t snap =
                    cells_[flat(rx, ry)].load(std::memory_order_relaxed) & kLayerMask;
                if (snap) fn(rx, ry, snap);
            }
        }
    }

    /**
     * @brief Invoke `fn(x, y, layer_mask)` for every non-empty cell in a box
     *        with layers intersecting [from, to].
     *
     * `layer_mask` is the full bitmap ANDed with the layer-range mask.
     *
     * @tparam Fn Callable matching `void(uint32_t, uint32_t, uint64_t)`.
     */
    template <typename Fn>
    void for_each_set_in_box(uint32_t x1, uint32_t y1,
                              uint32_t x2, uint32_t y2,
                              uint32_t from, uint32_t to,
                              Fn&& fn) const noexcept(noexcept(fn(0u, 0u, 0ull))) {
        assert(x1 <= x2 && x2 < W && y1 <= y2 && y2 < H);
        assert(from <= to && to < D);
        const uint64_t range_mask = detail::layer_range_mask(from, to);
        for (uint32_t ry = y1; ry <= y2; ++ry) {
            for (uint32_t rx = x1; rx <= x2; ++rx) {
                const uint64_t snap =
                    cells_[flat(rx, ry)].load(std::memory_order_relaxed) & range_mask;
                if (snap) fn(rx, ry, snap);
            }
        }
    }

    // ── Grid-level bitwise operations ─────────────────────────────────────────

    /**
     * @brief OR `other` into this grid (union of occupied cells).
     *
     * Use case: merge spawned entities into the main obstacle grid.
     * @param other Must have identical W, H, D template parameters.
     */
    void merge_from(const GridBitset& other) noexcept {
        for (size_t i = 0u; i < kCells; ++i)
            cells_[i].fetch_or(
                other.cells_[i].load(std::memory_order_relaxed) & kLayerMask,
                std::memory_order_release);
    }

    /**
     * @brief AND this grid with `other` (keep only cells set in both).
     *
     * Use case: compute overlap between two visibility sets.
     * @param other Must have identical W, H, D template parameters.
     */
    void intersect_with(const GridBitset& other) noexcept {
        for (size_t i = 0u; i < kCells; ++i) {
            const uint64_t keep =
                other.cells_[i].load(std::memory_order_relaxed) & kLayerMask;
            cells_[i].fetch_and(keep, std::memory_order_release);
        }
    }

    /**
     * @brief AND-NOT `other` from this grid (remove cells set in `other`).
     *
     * Use case: remove obstacles that were destroyed in a physics event.
     * @param other Must have identical W, H, D template parameters.
     */
    void diff_from(const GridBitset& other) noexcept {
        for (size_t i = 0u; i < kCells; ++i) {
            const uint64_t remove =
                other.cells_[i].load(std::memory_order_relaxed) & kLayerMask;
            cells_[i].fetch_and(~remove, std::memory_order_release);
        }
    }

    // ── Raycasting (DDA) ─────────────────────────────────────────────────────

    /**
     * @brief Ray-cast result returned by `raycast()`.
     */
    struct RayHit {
        uint32_t x;          ///< Hit cell column.
        uint32_t y;          ///< Hit cell row.
        uint64_t layer_mask; ///< Full layer bitmap of the hit cell (ANDed with range).
        uint32_t steps;      ///< Number of grid steps taken until the hit.
    };

    /**
     * @brief Walk a DDA ray from (x0, y0) in direction (dx, dy) and return
     *        the first cell that has any layer set in [from, to].
     *
     * Uses the integer DDA (Digital Differential Analyzer) algorithm —
     * no floating-point divisions after setup.
     *
     * @param x0, y0   Ray origin (must be inside the grid).
     * @param dx, dy   Direction components (integers; at least one non-zero).
     * @param from, to Layer range to test against.
     * @param max_steps Maximum grid cells to traverse before giving up.
     * @returns `RayHit` on first occupied cell, or `std::nullopt` if nothing
     *          was hit within `max_steps` steps.
     */
    [[nodiscard]] std::optional<RayHit>
    raycast(int32_t x0, int32_t y0,
            int32_t dx, int32_t dy,
            uint32_t from, uint32_t to,
            uint32_t max_steps) const noexcept {
        assert(dx != 0 || dy != 0);
        assert(from <= to && to < D);
        assert(x0 >= 0 && static_cast<uint32_t>(x0) < W);
        assert(y0 >= 0 && static_cast<uint32_t>(y0) < H);

        const uint64_t range_mask = detail::layer_range_mask(from, to);

        // DDA: step sign and absolute deltas.
        const int32_t sx = (dx > 0) ? 1 : (dx < 0 ? -1 : 0);
        const int32_t sy = (dy > 0) ? 1 : (dy < 0 ? -1 : 0);
        // Absolute component magnitudes (Chebyshev-step DDA).
        const int32_t adx = (dx < 0 ? -dx : dx);
        const int32_t ady = (dy < 0 ? -dy : dy);

        int32_t cx = x0, cy = y0;
        int32_t err = adx - ady; // Bresenham error accumulator

        for (uint32_t step = 0u; step < max_steps; ++step) {
            // Bounds check — stop if ray leaves the grid.
            if (cx < 0 || cy < 0 ||
                static_cast<uint32_t>(cx) >= W ||
                static_cast<uint32_t>(cy) >= H)
                return std::nullopt;

            const uint64_t snap =
                cells_[flat(static_cast<uint32_t>(cx),
                            static_cast<uint32_t>(cy))]
                    .load(std::memory_order_relaxed) & range_mask;
            if (snap)
                return RayHit{static_cast<uint32_t>(cx),
                              static_cast<uint32_t>(cy),
                              snap, step};

            // Advance DDA.
            const int32_t e2 = 2 * err;
            if (e2 > -ady) { err -= ady; cx += sx; }
            if (e2 <  adx) { err += adx; cy += sy; }
        }
        return std::nullopt;
    }

    // ── Metadata ─────────────────────────────────────────────────────────────

    /** @brief Total number of cells (W × H). */
    [[nodiscard]] static constexpr size_t cell_count() noexcept { return kCells; }

private:
    /** @brief Flat row-major index. */
    [[nodiscard]] static constexpr size_t flat(uint32_t x, uint32_t y) noexcept {
        return static_cast<size_t>(y) * W + x;
    }

    // 64-byte alignment ensures each row of 8 cells shares a cache line boundary.
    alignas(64) std::atomic<uint64_t> cells_[kCells];
};

// ── GridBitset2D — pure 2D bitset with Morton-code Super-Blocks ───────────────

/**
 * @brief Wait-free 2D bitset with Morton-order cache-line blocking.
 *
 * Bits are packed into `uint64_t` "Super-Blocks" of 8×8 = 64 cells.
 * Within each Super-Block, cells are stored in Morton (Z-curve) order,
 * guaranteeing that spatially close cells are also close in memory.
 *
 * Super-Block layout:
 * - W and H are rounded up to the next multiple of 8 internally.
 * - Total storage = `((W+7)/8) * ((H+7)/8)` words of `uint64_t`.
 *
 * Read operations are wait-free (single atomic load).
 * Write operations use `fetch_or`/`fetch_and` (lock-free).
 *
 * @tparam W Grid width  (cells). Need not be a multiple of 8.
 * @tparam H Grid height (cells). Need not be a multiple of 8.
 */
template <uint32_t W, uint32_t H>
class GridBitset2D {
    static_assert(W > 0u, "Grid width must be positive");
    static_assert(H > 0u, "Grid height must be positive");

    // Super-Block dimensions: 8×8 = 64 bits per uint64_t word.
    static constexpr uint32_t kSBSize = 8u;
    // Number of Super-Blocks in each axis.
    static constexpr uint32_t kSBW = (W + kSBSize - 1u) / kSBSize;
    static constexpr uint32_t kSBH = (H + kSBSize - 1u) / kSBSize;
    // Total super-block count.
    static constexpr size_t kSBCount = static_cast<size_t>(kSBW) * kSBH;

public:
    static constexpr uint32_t kWidth  = W;
    static constexpr uint32_t kHeight = H;

    // ── Construction ─────────────────────────────────────────────────────────

    /** @brief Zero-initialize (all cells unoccupied). */
    GridBitset2D() noexcept {
        for (auto& w : words_)
            w.store(uint64_t{0}, std::memory_order_relaxed);
    }

    GridBitset2D(const GridBitset2D&)            = delete;
    GridBitset2D& operator=(const GridBitset2D&) = delete;

    // ── Write operations ─────────────────────────────────────────────────────

    /**
     * @brief Set (occupy) cell (x, y).
     * @param x Column [0, W). @param y Row [0, H).
     */
    void set(uint32_t x, uint32_t y) noexcept {
        assert(x < W && y < H);
        auto [word_idx, bit_idx] = locate(x, y);
        words_[word_idx].fetch_or(uint64_t{1} << bit_idx,
                                  std::memory_order_release);
    }

    /**
     * @brief Clear (vacate) cell (x, y).
     * @param x Column [0, W). @param y Row [0, H).
     */
    void clear(uint32_t x, uint32_t y) noexcept {
        assert(x < W && y < H);
        auto [word_idx, bit_idx] = locate(x, y);
        words_[word_idx].fetch_and(~(uint64_t{1} << bit_idx),
                                   std::memory_order_release);
    }

    /** @brief Reset all cells to zero. */
    void reset() noexcept {
        for (auto& w : words_)
            w.store(uint64_t{0}, std::memory_order_relaxed);
    }

    // ── Read operations (wait-free) ───────────────────────────────────────────

    /**
     * @brief Test whether cell (x, y) is occupied. ~2 ns.
     */
    [[nodiscard]] bool test(uint32_t x, uint32_t y) const noexcept {
        assert(x < W && y < H);
        auto [word_idx, bit_idx] = locate(x, y);
        return (words_[word_idx].load(std::memory_order_acquire) >> bit_idx) & 1u;
    }

    /**
     * @brief Check whether any cell in a box is occupied.
     *
     * Iterates at Super-Block granularity: if an entire SB word is zero
     * the block is skipped in O(1), otherwise individual bits are tested.
     *
     * @returns true on first occupied cell found.
     */
    [[nodiscard]] bool any_in_box(uint32_t x1, uint32_t y1,
                                  uint32_t x2, uint32_t y2) const noexcept {
        assert(x1 <= x2 && x2 < W && y1 <= y2 && y2 < H);
        for (uint32_t ry = y1; ry <= y2; ++ry) {
            for (uint32_t rx = x1; rx <= x2; ++rx) {
                auto [word_idx, bit_idx] = locate(rx, ry);
                if ((words_[word_idx].load(std::memory_order_relaxed) >> bit_idx) & 1u)
                    return true;
            }
        }
        return false;
    }

    /**
     * @brief Return the raw 64-bit Super-Block containing (x, y).
     *
     * The Super-Block's bit layout is Morton-ordered within an 8×8 region.
     * Use `detail::morton2d_decode()` to interpret individual bits.
     */
    [[nodiscard]] uint64_t super_block_snapshot(uint32_t x, uint32_t y) const noexcept {
        assert(x < W && y < H);
        const size_t sb_idx = super_block_index(x, y);
        return words_[sb_idx].load(std::memory_order_acquire);
    }

    // ── Bit flip ─────────────────────────────────────────────────────────────

    /**
     * @brief Atomically toggle cell (x, y) — XOR 1 bit.
     *
     * Useful for toggling gates, destructible tiles, switches.
     * @returns true if the bit is now set (after the toggle).
     */
    bool toggle(uint32_t x, uint32_t y) noexcept {
        assert(x < W && y < H);
        auto [word_idx, bit_idx] = locate(x, y);
        const uint64_t bit  = uint64_t{1} << bit_idx;
        const uint64_t prev = words_[word_idx].fetch_xor(bit,
                                                         std::memory_order_acq_rel);
        return (prev & bit) == 0u;
    }

    // ── Bulk operations ───────────────────────────────────────────────────────

    /**
     * @brief Clear all cells in the axis-aligned box [x1,x2]×[y1,y2].
     *
     * Useful for room resets, explosion clearing, door openings.
     */
    void clear_box(uint32_t x1, uint32_t y1,
                   uint32_t x2, uint32_t y2) noexcept {
        assert(x1 <= x2 && x2 < W && y1 <= y2 && y2 < H);
        for (uint32_t ry = y1; ry <= y2; ++ry)
            for (uint32_t rx = x1; rx <= x2; ++rx)
                clear(rx, ry);
    }

    // ── Aggregates ────────────────────────────────────────────────────────────

    /**
     * @brief Count all occupied cells in the grid.
     *
     * Sums POPCNT over all Super-Block words.  O(W×H / 64).
     */
    [[nodiscard]] uint32_t count_all() const noexcept {
        uint32_t total = 0u;
        for (const auto& w : words_)
            total += static_cast<uint32_t>(
                std::popcount(w.load(std::memory_order_relaxed)));
        return total;
    }

    /**
     * @brief Count occupied cells in a box.
     */
    [[nodiscard]] uint32_t count_in_box(uint32_t x1, uint32_t y1,
                                        uint32_t x2, uint32_t y2) const noexcept {
        assert(x1 <= x2 && x2 < W && y1 <= y2 && y2 < H);
        uint32_t n = 0u;
        for (uint32_t ry = y1; ry <= y2; ++ry)
            for (uint32_t rx = x1; rx <= x2; ++rx)
                if (test(rx, ry)) ++n;
        return n;
    }

    // ── Iteration ─────────────────────────────────────────────────────────────

    /**
     * @brief Invoke `fn(x, y)` for every occupied cell in the grid.
     *
     * Super-Block words that are zero are skipped in O(1), so sparse grids
     * are traversed efficiently.
     *
     * @tparam Fn Callable matching `void(uint32_t x, uint32_t y)`.
     */
    template <typename Fn>
    void for_each_set(Fn&& fn) const noexcept(noexcept(fn(0u, 0u))) {
        for (uint32_t sby = 0u; sby < kSBH; ++sby) {
            for (uint32_t sbx = 0u; sbx < kSBW; ++sbx) {
                uint64_t word =
                    words_[static_cast<size_t>(sby) * kSBW + sbx]
                        .load(std::memory_order_relaxed);
                if (!word) continue;
                while (word) {
                    const uint32_t bit  = static_cast<uint32_t>(std::countr_zero(word));
                    uint32_t lx{}, ly{};
                    detail::morton2d_decode(bit, lx, ly);
                    const uint32_t gx = sbx * kSBSize + lx;
                    const uint32_t gy = sby * kSBSize + ly;
                    if (gx < W && gy < H) fn(gx, gy);
                    word &= word - 1u; // clear lowest set bit
                }
            }
        }
    }

    /**
     * @brief Invoke `fn(x, y)` for every occupied cell in a box.
     *
     * @tparam Fn Callable matching `void(uint32_t x, uint32_t y)`.
     */
    template <typename Fn>
    void for_each_set_in_box(uint32_t x1, uint32_t y1,
                              uint32_t x2, uint32_t y2,
                              Fn&& fn) const noexcept(noexcept(fn(0u, 0u))) {
        assert(x1 <= x2 && x2 < W && y1 <= y2 && y2 < H);
        for (uint32_t ry = y1; ry <= y2; ++ry)
            for (uint32_t rx = x1; rx <= x2; ++rx)
                if (test(rx, ry)) fn(rx, ry);
    }

    // ── Grid-level bitwise operations ─────────────────────────────────────────

    /**
     * @brief OR `other` into this grid (union).
     *
     * Use case: merge an entity's footprint into the collision map.
     */
    void merge_from(const GridBitset2D& other) noexcept {
        for (size_t i = 0u; i < kSBCount; ++i)
            words_[i].fetch_or(other.words_[i].load(std::memory_order_relaxed),
                               std::memory_order_release);
    }

    /**
     * @brief AND this grid with `other` (intersection).
     *
     * Use case: retain only cells visible to both player and enemy.
     */
    void intersect_with(const GridBitset2D& other) noexcept {
        for (size_t i = 0u; i < kSBCount; ++i)
            words_[i].fetch_and(other.words_[i].load(std::memory_order_relaxed),
                                std::memory_order_release);
    }

    /**
     * @brief AND-NOT `other` from this grid (remove cells in `other`).
     *
     * Use case: remove destroyed obstacles from the collision map.
     */
    void diff_from(const GridBitset2D& other) noexcept {
        for (size_t i = 0u; i < kSBCount; ++i) {
            const uint64_t remove = other.words_[i].load(std::memory_order_relaxed);
            words_[i].fetch_and(~remove, std::memory_order_release);
        }
    }

    // ── Raycasting (Bresenham DDA) ────────────────────────────────────────────

    /**
     * @brief Result returned by `raycast_2d()`.
     */
    struct RayHit {
        uint32_t x;      ///< Hit cell column.
        uint32_t y;      ///< Hit cell row.
        uint32_t steps;  ///< Steps taken until the hit.
    };

    /**
     * @brief Walk a Bresenham ray from (x0,y0) toward (x1,y1) and return
     *        the first occupied cell along the path.
     *
     * Terminates at the target cell or grid boundary.
     * Useful for line-of-sight, bullet traces, and corridor detection.
     *
     * @returns `RayHit` on first hit, or `std::nullopt` if the path is clear.
     */
    [[nodiscard]] std::optional<RayHit>
    raycast_2d(uint32_t x0, uint32_t y0,
               uint32_t x1, uint32_t y1) const noexcept {
        assert(x0 < W && y0 < H && x1 < W && y1 < H);

        const int32_t dx  = (int32_t)x1 - (int32_t)x0;
        const int32_t dy  = (int32_t)y1 - (int32_t)y0;
        const int32_t sx  = (dx > 0) ? 1 : -1;
        const int32_t sy  = (dy > 0) ? 1 : -1;
        int32_t adx = (dx < 0 ? -dx : dx);
        int32_t ady = (dy < 0 ? -dy : dy);
        int32_t err = adx - ady;
        int32_t cx  = (int32_t)x0;
        int32_t cy  = (int32_t)y0;

        const uint32_t max_steps = static_cast<uint32_t>(adx + ady + 1u);
        for (uint32_t s = 0u; s < max_steps; ++s) {
            if (test(static_cast<uint32_t>(cx), static_cast<uint32_t>(cy)))
                return RayHit{static_cast<uint32_t>(cx),
                              static_cast<uint32_t>(cy), s};
            if (cx == (int32_t)x1 && cy == (int32_t)y1) break;
            const int32_t e2 = 2 * err;
            if (e2 > -ady) { err -= ady; cx += sx; }
            if (e2 <  adx) { err += adx; cy += sy; }
        }
        return std::nullopt;
    }

    /** @brief Total cells (W × H). */
    [[nodiscard]] static constexpr size_t cell_count() noexcept {
        return static_cast<size_t>(W) * H;
    }

private:
    struct Location { size_t word_idx; uint32_t bit_idx; };

    /**
     * @brief Compute the Super-Block word index and bit position for (x, y).
     *
     * Within each 8×8 Super-Block, cells are indexed by their 2D Morton code
     * so spatially adjacent cells share cache lines.
     */
    [[nodiscard]] static Location locate(uint32_t x, uint32_t y) noexcept {
        // Super-Block coordinates
        const uint32_t sbx = x / kSBSize;
        const uint32_t sby = y / kSBSize;
        // Local coordinates within the Super-Block [0, 7]
        const uint32_t lx  = x % kSBSize;
        const uint32_t ly  = y % kSBSize;
        // Morton code within the Super-Block (6 bits for 8×8)
        const uint32_t morton = detail::morton2d(lx, ly);
        // Flat Super-Block index (row-major over super-blocks)
        const size_t sb_idx = static_cast<size_t>(sby) * kSBW + sbx;
        return {sb_idx, morton & 63u};
    }

    [[nodiscard]] static size_t super_block_index(uint32_t x, uint32_t y) noexcept {
        return static_cast<size_t>(y / kSBSize) * kSBW + (x / kSBSize);
    }

    alignas(64) std::atomic<uint64_t> words_[kSBCount];
};

} // namespace qbuem
