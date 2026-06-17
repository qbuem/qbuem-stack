#pragma once
// ============================================================================
// spatial_grid.hpp — fixed spatial bucket grid: cell → object(s).
//
// The OBJECT-index companion to GridBitset (which stores presence BITS only). A
// game server's area-of-interest / broad-phase needs to map each (x, y, layer) to
// the LIST of objects there — including several objects in one cell — which a
// bitset cannot. SpatialGrid buckets objects by a coarse cell; rebuild it each
// tick (clear + insert), then `for_each_in_radius` to enumerate candidates for an
// AOI query or a collision broad phase (the caller applies the exact test).
//
// Buckets keep their capacity across clear(), so after warm-up a rebuild is 0
// heap allocations. Single-threaded use (rebuild + query on the owning thread).
//
//   SpatialGrid<Player, 128, 128, /*Layers=*/8, /*BucketSize=*/16> grid;
//   grid.clear();
//   for (auto& p : players) grid.insert(p.x, p.y, p.floor, &p);
//   grid.for_each_in_radius(me.x, me.y, me.floor, 8, [&](Player* o){ /* exact test */ });
// ============================================================================
#include <cassert>
#include <cstdint>
#include <vector>

namespace qbuem {

template <class T, std::uint32_t Width, std::uint32_t Height,
          std::uint32_t Layers = 1, std::uint32_t BucketSize = 16>
class SpatialGrid {
    static_assert(Width > 0 && Height > 0 && Layers >= 1 && BucketSize >= 1,
                  "SpatialGrid dimensions must be positive");

public:
    static constexpr std::uint32_t kCols            = (Width  + BucketSize - 1) / BucketSize;  // ceil
    static constexpr std::uint32_t kRows            = (Height + BucketSize - 1) / BucketSize;
    static constexpr std::uint32_t kBucketsPerLayer = kCols * kRows;
    static constexpr std::uint32_t kBucketCount     = kBucketsPerLayer * Layers;

    SpatialGrid() : buckets_(kBucketCount) {}

    /** @brief Empty every bucket, retaining capacity (0 alloc after warm-up). */
    void clear() noexcept { for (auto& b : buckets_) b.clear(); }

    /** @brief Index `obj` at cell (x, y) on `layer`. Multiple objects per cell ok. */
    void insert(std::uint32_t x, std::uint32_t y, std::uint32_t layer, T* obj) {
        assert(x < Width && y < Height && layer < Layers);
        buckets_[bucket_of(x / BucketSize, y / BucketSize, layer)].push_back(obj);
    }

    /** @brief Broad phase: invoke `fn(T*)` for every object whose bucket overlaps
     *  the square of Chebyshev radius `r` around (x, y) on `layer`. Includes every
     *  object within `r` (no false negatives); may include some farther ones (the
     *  caller applies the exact distance/LoS test). Each object is visited once.
     *  Correct for any BucketSize; tune BucketSize ≈ the typical query radius so the
     *  bucket span stays small (≈ 3×3). */
    template <class Fn>
    void for_each_in_radius(std::uint32_t x, std::uint32_t y, std::uint32_t layer,
                            std::uint32_t r, Fn&& fn) const {
        assert(layer < Layers);
        const int bx0 = clampi((int(x) - int(r)) / int(BucketSize), 0, int(kCols) - 1);
        const int bx1 = clampi((int(x) + int(r)) / int(BucketSize), 0, int(kCols) - 1);
        const int by0 = clampi((int(y) - int(r)) / int(BucketSize), 0, int(kRows) - 1);
        const int by1 = clampi((int(y) + int(r)) / int(BucketSize), 0, int(kRows) - 1);
        for (int by = by0; by <= by1; ++by)
            for (int bx = bx0; bx <= bx1; ++bx)
                for (T* obj : buckets_[bucket_of(std::uint32_t(bx), std::uint32_t(by), layer)])
                    fn(obj);
    }

private:
    static constexpr std::uint32_t bucket_of(std::uint32_t bx, std::uint32_t by, std::uint32_t layer) noexcept {
        return layer * kBucketsPerLayer + by * kCols + bx;
    }
    static constexpr int clampi(int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

    std::vector<std::vector<T*>> buckets_;
};

}  // namespace qbuem
