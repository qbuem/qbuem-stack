/**
 * @file tests/grid_spatial_test.cpp
 * @brief Tests for GridBitset radius queries and TiledBitset.
 *
 * Covers:
 *  - GridBitset: any_in_radius / count_in_radius / for_each_set_in_radius
 *  - TiledBitset: basic ops, negative coords, cross-tile box/radius/raycast,
 *                 evict_empty_tiles, coordinate helpers
 */

#include <gtest/gtest.h>

#include <qbuem/buf/grid_bitset.hpp>
#include <qbuem/buf/tiled_bitset.hpp>

using namespace qbuem;

// ── GridBitset radius queries ─────────────────────────────────────────────────

class GridRadiusTest : public ::testing::Test {
protected:
    static constexpr uint32_t W = 128, H = 128, D = 16;
    GridBitset<W, H, D> grid{};

    void SetUp() override { grid.reset(); }
};

TEST_F(GridRadiusTest, AnyInRadiusEmpty) {
    EXPECT_FALSE(grid.any_in_radius(64, 64, 10, 0, D - 1));
}

TEST_F(GridRadiusTest, AnyInRadiusHit) {
    grid.set(64, 64, 0);                                   // center
    EXPECT_TRUE(grid.any_in_radius(64, 64, 0, 0, D - 1)); // r=0: center only
    EXPECT_TRUE(grid.any_in_radius(60, 60, 8, 0, D - 1)); // r=8, within range
}

TEST_F(GridRadiusTest, AnyInRadiusMiss) {
    grid.set(10, 10, 3);
    EXPECT_FALSE(grid.any_in_radius(64, 64, 5, 0, D - 1)); // too far away
}

TEST_F(GridRadiusTest, AnyInRadiusLayerFilter) {
    grid.set(64, 64, 5); // layer 5
    EXPECT_FALSE(grid.any_in_radius(64, 64, 1, 0, 4)); // layers 0-4: miss
    EXPECT_TRUE(grid.any_in_radius(64, 64, 1, 5, 9));  // layer 5 in range
}

TEST_F(GridRadiusTest, CountInRadiusZero) {
    EXPECT_EQ(grid.count_in_radius(64, 64, 10, 0, D - 1), 0u);
}

TEST_F(GridRadiusTest, CountInRadiusSingleCell) {
    grid.set(64, 64, 2);
    EXPECT_EQ(grid.count_in_radius(64, 64, 0, 0, D - 1), 1u); // r=0 → only center
    EXPECT_EQ(grid.count_in_radius(64, 64, 5, 0, D - 1), 1u); // r=5, still 1 set cell
}

TEST_F(GridRadiusTest, CountInRadiusMultipleCells) {
    // Place 5 cells in a + pattern (center + 4 cardinal neighbors)
    grid.set(50, 50, 0);
    grid.set(51, 50, 0);
    grid.set(49, 50, 0);
    grid.set(50, 51, 0);
    grid.set(50, 49, 0);

    EXPECT_EQ(grid.count_in_radius(50, 50, 1, 0, D - 1), 5u); // all 5 within r=1
    EXPECT_EQ(grid.count_in_radius(50, 50, 0, 0, D - 1), 1u); // only center
}

TEST_F(GridRadiusTest, CountInRadiusApproxCircleArea) {
    // Fill the entire grid, count cells in radius r=20: should match π×r²
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x)
            grid.set(x, y, 0);

    const uint32_t r   = 20u;
    const uint32_t cnt = grid.count_in_radius(64, 64, r, 0, 0);
    // π × 20² ≈ 1257; allow ±5 for sqrt rounding
    EXPECT_GE(cnt, 1250u);
    EXPECT_LE(cnt, 1270u);
}

TEST_F(GridRadiusTest, ForEachSetInRadiusAllWithinRadius) {
    // Place cells at known offsets; verify every visited cell is within radius.
    grid.set(60, 60, 1);
    grid.set(65, 62, 0);
    grid.set(70, 70, 3); // outside r=8 from (64,64)

    const uint32_t cx = 64, cy = 64, r = 8;
    uint32_t visited = 0;
    grid.for_each_set_in_radius(cx, cy, r, 0, D - 1,
        [&](uint32_t x, uint32_t y, uint64_t) {
            const int32_t dx = static_cast<int32_t>(x) - static_cast<int32_t>(cx);
            const int32_t dy = static_cast<int32_t>(y) - static_cast<int32_t>(cy);
            EXPECT_LE(dx * dx + dy * dy,
                      static_cast<int32_t>(r * r) + 1); // within circle
            ++visited;
        });

    EXPECT_EQ(visited, 2u); // (60,60) and (65,62) are within r=8; (70,70) is not
}

TEST_F(GridRadiusTest, RadiusClampedToGridBoundary) {
    grid.set(2, 2, 0); // near top-left corner
    // Large radius from corner — must not access out-of-bounds cells
    EXPECT_TRUE(grid.any_in_radius(0, 0, 5, 0, D - 1));
    EXPECT_GE(grid.count_in_radius(0, 0, 5, 0, D - 1), 1u);
}

// ── TiledBitset basic ops ─────────────────────────────────────────────────────

using World = TiledBitset<64, 64, 16>;

class TiledBitsetTest : public ::testing::Test {
protected:
    World world{};
};

TEST_F(TiledBitsetTest, SetAndTest) {
    world.set(100, 200, 3);
    EXPECT_TRUE(world.test(100, 200, 3));
    EXPECT_FALSE(world.test(100, 200, 4));
    EXPECT_FALSE(world.test(101, 200, 3));
}

TEST_F(TiledBitsetTest, SetAndClear) {
    world.set(10, 10, 0);
    EXPECT_TRUE(world.test(10, 10, 0));
    world.clear(10, 10, 0);
    EXPECT_FALSE(world.test(10, 10, 0));
}

TEST_F(TiledBitsetTest, TestUnloadedTileReturnsFalse) {
    // Never written to — tile should not be allocated, test returns false.
    EXPECT_FALSE(world.test(99999, 99999, 0));
    EXPECT_EQ(world.loaded_tile_count(), 0u);
}

TEST_F(TiledBitsetTest, SnapshotUnloadedReturnsZero) {
    EXPECT_EQ(world.snapshot(500, 500), 0u);
}

TEST_F(TiledBitsetTest, NegativeCoordinates) {
    world.set(-1,   -1,   7);
    world.set(-100, -200, 2);
    world.set(-64,   0,   0); // exactly at tile boundary

    EXPECT_TRUE(world.test(-1,   -1,   7));
    EXPECT_TRUE(world.test(-100, -200, 2));
    EXPECT_TRUE(world.test(-64,   0,   0));

    EXPECT_FALSE(world.test(-1, -1, 6));
    EXPECT_FALSE(world.test(0,  0,  7)); // different cell
}

TEST_F(TiledBitsetTest, NegativeAndPositiveSameTile) {
    // -1 and -64 are in tile (-1, *) for TileW=64; 0 is in tile (0, *)
    world.set(-1,  10, 5);
    world.set( 0,  10, 5);

    EXPECT_TRUE(world.test(-1, 10, 5));
    EXPECT_TRUE(world.test( 0, 10, 5));
    EXPECT_EQ(world.loaded_tile_count(), 2u); // two different tiles
}

TEST_F(TiledBitsetTest, CoordinateHelperRoundtrip) {
    // Verify world → tile → local → world roundtrip for various coords.
    for (World::WorldCoord w : {World::WorldCoord{0}, World::WorldCoord{63}, World::WorldCoord{64}, World::WorldCoord{127}, World::WorldCoord{-1}, World::WorldCoord{-64}, World::WorldCoord{-65}, World::WorldCoord{1000}, World::WorldCoord{-1000}}) {
        const auto tx = World::tile_x(w);
        const auto lx = World::local_x(w);
        EXPECT_EQ(World::world_x(tx, lx), w)
            << "roundtrip failed for wx=" << w;
        EXPECT_LT(lx, World::kTileW);

        const auto ty = World::tile_y(w);
        const auto ly = World::local_y(w);
        EXPECT_EQ(World::world_y(ty, ly), w)
            << "roundtrip failed for wy=" << w;
        EXPECT_LT(ly, World::kTileH);
    }
}

TEST_F(TiledBitsetTest, TileCount) {
    world.set(0, 0, 0);   // tile (0,0)
    world.set(64, 0, 0);  // tile (1,0)
    world.set(0, 64, 0);  // tile (0,1)
    EXPECT_EQ(world.loaded_tile_count(), 3u);
}

TEST_F(TiledBitsetTest, Toggle) {
    EXPECT_TRUE(world.toggle(50, 50, 3));  // off → on, returns true
    EXPECT_TRUE(world.test(50, 50, 3));
    EXPECT_FALSE(world.toggle(50, 50, 3)); // on → off, returns false
    EXPECT_FALSE(world.test(50, 50, 3));
}

TEST_F(TiledBitsetTest, AnyInRange) {
    world.set(10, 10, 5);
    EXPECT_TRUE(world.any_in_range(10, 10, 4, 6));
    EXPECT_FALSE(world.any_in_range(10, 10, 0, 4));
    EXPECT_FALSE(world.any_in_range(10, 10, 6, 10));
}

// ── TiledBitset box queries ───────────────────────────────────────────────────

TEST_F(TiledBitsetTest, AnyInBoxSingleTile) {
    world.set(10, 10, 0);
    EXPECT_TRUE(world.any_in_box(5, 5, 20, 20, 0, 15));
    EXPECT_FALSE(world.any_in_box(20, 20, 30, 30, 0, 15));
}

TEST_F(TiledBitsetTest, AnyInBoxCrossTile) {
    world.set(63, 63, 0); // tile (0,0) corner
    world.set(64, 64, 0); // tile (1,1) corner

    // Box spanning all four tiles around the boundary
    EXPECT_TRUE(world.any_in_box(60, 60, 70, 70, 0, 15));
    // Narrow box that misses both cells
    EXPECT_FALSE(world.any_in_box(64, 63, 70, 63, 0, 15));
}

TEST_F(TiledBitsetTest, CountInBoxSingleTile) {
    world.set(5, 5, 0);
    world.set(6, 5, 0);
    world.set(7, 6, 0);
    EXPECT_EQ(world.count_in_box(0, 0, 10, 10, 0, 15), 3u);
    EXPECT_EQ(world.count_in_box(8, 0, 10, 10, 0, 15), 0u);
}

TEST_F(TiledBitsetTest, CountInBoxCrossTile) {
    // One cell in tile (-1,-1) and one in tile (0,0)
    world.set(-1, -1, 2);
    world.set( 0,  0, 2);
    EXPECT_EQ(world.count_in_box(-5, -5, 5, 5, 2, 2), 2u);
}

TEST_F(TiledBitsetTest, CountInBoxUnloadedRegionIsZero) {
    EXPECT_EQ(world.count_in_box(10000, 10000, 10100, 10100, 0, 15), 0u);
    EXPECT_EQ(world.loaded_tile_count(), 0u); // query must not create tiles
}

// ── TiledBitset radius queries ────────────────────────────────────────────────

TEST_F(TiledBitsetTest, AnyInRadiusSingleTile) {
    world.set(30, 30, 1);
    EXPECT_TRUE(world.any_in_radius(30, 30, 5, 0, 15));
    EXPECT_FALSE(world.any_in_radius(100, 100, 5, 0, 15));
}

TEST_F(TiledBitsetTest, AnyInRadiusCrossTile) {
    // Cell at (63, 32): in tile (0,0). Center of radius at (65,32): in tile(1,0).
    world.set(63, 32, 0); // distance 2 from (65,32)
    EXPECT_TRUE(world.any_in_radius(65, 32, 3, 0, 15));
    EXPECT_FALSE(world.any_in_radius(65, 32, 1, 0, 15)); // distance 2 > r=1
}

TEST_F(TiledBitsetTest, CountInRadiusCrossTile) {
    // Place cells symmetrically around tile boundary at (64,64)
    world.set(60, 64, 0);
    world.set(64, 60, 0);
    world.set(68, 64, 0);
    world.set(64, 68, 0);

    // All four cells are at distance ~4 from (64,64) → within r=5
    EXPECT_EQ(world.count_in_radius(64, 64, 5, 0, 15), 4u);
    // Only cells within r=3 → none (distances are all 4)
    EXPECT_EQ(world.count_in_radius(64, 64, 3, 0, 15), 0u);
}

TEST_F(TiledBitsetTest, ForEachSetInRadius) {
    world.set(10, 10, 0);
    world.set(11, 10, 0);
    world.set(20, 20, 0); // outside r=5 from (10,10)

    uint32_t count = 0;
    world.for_each_set_in_radius(10, 10, 5, 0, 15,
        [&](World::WorldCoord wx, World::WorldCoord wy, uint64_t) {
            const int64_t dx = wx - 10, dy = wy - 10;
            EXPECT_LE(dx * dx + dy * dy, 25LL); // within r=5
            ++count;
        });
    EXPECT_EQ(count, 2u);
}

// ── TiledBitset raycast ───────────────────────────────────────────────────────

TEST_F(TiledBitsetTest, RaycastHitInSameTile) {
    world.set(50, 0, 0);
    auto r = world.raycast(0, 0, 1, 0, 0, 15, 200);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->wx, 50);
    EXPECT_EQ(r->wy, 0);
    EXPECT_EQ(r->steps, 50u);
}

TEST_F(TiledBitsetTest, RaycastCrossTile) {
    // Tile boundary is at x=64. Place obstacle just past it.
    world.set(70, 0, 5);
    auto r = world.raycast(0, 0, 1, 0, 5, 5, 200);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->wx, 70);
}

TEST_F(TiledBitsetTest, RaycastNegativeDirection) {
    world.set(-30, 0, 0);
    auto r = world.raycast(0, 0, -1, 0, 0, 15, 200);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->wx, -30);
}

TEST_F(TiledBitsetTest, RaycastMiss) {
    auto r = world.raycast(0, 0, 1, 0, 0, 15, 100);
    EXPECT_FALSE(r.has_value());
}

TEST_F(TiledBitsetTest, RaycastDiagonal) {
    world.set(10, 10, 0);
    auto r = world.raycast(0, 0, 1, 1, 0, 15, 30);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->wx, 10);
    EXPECT_EQ(r->wy, 10);
}

// ── TiledBitset lifecycle ─────────────────────────────────────────────────────

TEST_F(TiledBitsetTest, EvictEmptyTiles) {
    world.set(0, 0, 3);
    world.set(64, 64, 5);
    EXPECT_EQ(world.loaded_tile_count(), 2u);

    world.clear(0, 0, 3);
    size_t evicted = world.evict_empty_tiles();
    // One tile became empty after clear
    EXPECT_EQ(evicted, 1u);
    EXPECT_EQ(world.loaded_tile_count(), 1u);
    // Remaining tile still valid
    EXPECT_TRUE(world.test(64, 64, 5));
}

TEST_F(TiledBitsetTest, ResetAll) {
    world.set(0,   0,  0);
    world.set(100, 100, 0);
    EXPECT_EQ(world.loaded_tile_count(), 2u);
    world.reset_all();
    EXPECT_EQ(world.loaded_tile_count(), 0u);
    EXPECT_FALSE(world.test(0, 0, 0));
}

TEST_F(TiledBitsetTest, HasTile) {
    EXPECT_FALSE(world.has_tile(0, 0));
    world.set(10, 10, 0);
    EXPECT_TRUE(world.has_tile(0, 0));
    EXPECT_FALSE(world.has_tile(1, 0));
}

TEST_F(TiledBitsetTest, MemoryBytes) {
    EXPECT_EQ(world.memory_bytes(), 0u);
    world.set(0, 0, 0);
    EXPECT_EQ(world.memory_bytes(), sizeof(World::Tile));
    world.set(64, 64, 0);
    EXPECT_EQ(world.memory_bytes(), 2u * sizeof(World::Tile));
}

TEST_F(TiledBitsetTest, ForEachSetInBox) {
    world.set(5,  5,  1);
    world.set(10, 10, 2);
    world.set(50, 50, 3); // outside box

    uint32_t count = 0;
    world.for_each_set_in_box(0, 0, 20, 20, 0, 15,
        [&](World::WorldCoord, World::WorldCoord, uint64_t) { ++count; });
    EXPECT_EQ(count, 2u);
}

TEST_F(TiledBitsetTest, ForEachTile) {
    world.set(0,  0, 0);  // tile (0,0)
    world.set(64, 0, 0);  // tile (1,0)

    uint32_t tile_count = 0;
    world.for_each_tile([&](World::TileCoord, World::TileCoord,
                            const World::Tile&) { ++tile_count; });
    EXPECT_EQ(tile_count, 2u);
}

TEST_F(TiledBitsetTest, GetTileAndPrefetch) {
    EXPECT_EQ(world.get_tile(0, 0), nullptr);
    world.prefetch_tile(0, 0);
    EXPECT_NE(world.get_tile(0, 0), nullptr);
}
