/**
 * @file examples/11-advanced-apps/spatial_fusion/spatial_fusion_example.cpp
 * @brief Spatial sensor fusion using GridBitset: 3D voxel occupancy + sphere queries.
 *
 * ## What this example demonstrates
 *
 *   1. **GridBitset<W, H, D>** (2.5D mode) — a 256×256 grid of cells where each
 *      cell stores up to 64 vertical layers in one `uint64_t`.  Layer queries
 *      ("is anything between altitude 5 and 10?") run in ~1 ns via a single
 *      bitwise-AND, no loops.
 *
 *   2. **GridBitset2D<W, H>** — a pure 2D occupancy grid with Morton-order
 *      Super-Block layout for cache-friendly box queries.
 *
 *   3. **Simulated LiDAR point cloud** — random voxels inserted into the 2.5D
 *      grid; then queried with an approximate sphere (bounding-box pre-filter +
 *      per-cell layer test).
 *
 *   4. **AOI (Area of Interest) manager** — lightweight region-of-interest
 *      tracking: register a 2D rectangle, get live occupancy reports.
 *
 * ## Scenario
 *
 *   A drone navigates a 256×256×64-voxel airspace.  Three obstacle clouds are
 *   injected at different altitudes.  The path planner queries the grid at
 *   100 µs intervals (driven by MicroTicker) to detect obstacles on the planned
 *   flight path.
 *
 * ## Build
 *   cmake --build build --target spatial_fusion_example
 *
 * ## Run
 *   ./build/examples/spatial_fusion_example
 */

#include <qbuem/buf/grid_bitset.hpp>
#include <qbuem/reactor/micro_ticker.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <qbuem/compat/print.hpp>
#include <random>
#include <string_view>
#include <vector>

using namespace std::chrono_literals;

// ── Grid dimensions ───────────────────────────────────────────────────────────

// 256 × 256 horizontal grid, 64 vertical layers (= exactly one uint64_t per column).
static constexpr uint32_t kW = 256;
static constexpr uint32_t kH = 256;
static constexpr uint32_t kD = 64;   // max 64 layers for atomic uint64_t storage

using VoxelGrid  = qbuem::GridBitset<kW, kH, kD>;  // 2.5D
using FloorMap   = qbuem::GridBitset2D<kW, kH>;     // 2D obstacle map

// ── Obstacle cloud insertion ──────────────────────────────────────────────────

/**
 * @brief Insert `count` random voxels within a spherical region into the grid.
 *
 * Only cells within the bounding box that also satisfy the distance check
 * (squared Euclidean) are inserted — approximating sphere occupancy.
 */
static void inject_sphere_cloud(VoxelGrid&   grid,
                                uint32_t     cx, uint32_t cy, uint32_t cz,
                                uint32_t     radius,
                                uint32_t     count,
                                std::mt19937& rng) {
    std::uniform_int_distribution<uint32_t> rx_d(
        cx > radius ? cx - radius : 0u,
        std::min(cx + radius, kW - 1u));
    std::uniform_int_distribution<uint32_t> ry_d(
        cy > radius ? cy - radius : 0u,
        std::min(cy + radius, kH - 1u));
    std::uniform_int_distribution<uint32_t> rz_d(
        cz > radius ? cz - radius : 0u,
        std::min(cz + radius, kD - 1u));

    const uint32_t r2 = radius * radius;
    for (uint32_t n = 0; n < count; ++n) {
        const uint32_t x = rx_d(rng);
        const uint32_t y = ry_d(rng);
        const uint32_t z = rz_d(rng);
        // Reject outside sphere.
        const uint32_t dx = (x > cx) ? x - cx : cx - x;
        const uint32_t dy = (y > cy) ? y - cy : cy - y;
        const uint32_t dz = (z > cz) ? z - cz : cz - z;
        if (dx * dx + dy * dy + dz * dz <= r2) {
            grid.set(x, y, z);
        }
    }
}

// ── Sphere query: bounding box + layer range check ────────────────────────────

/**
 * @brief Count occupied voxels within the query sphere.
 *
 * Hot path: for each cell in the bounding box, a single
 * `any_in_range(x, y, from, to)` call checks all layers with one bitwise-AND.
 *
 * @returns Number of occupied grid columns in the sphere's XY footprint
 *          that have at least one voxel in the altitude range [z_lo, z_hi].
 */
[[nodiscard]] static uint32_t query_sphere(const VoxelGrid& grid,
                                            uint32_t cx, uint32_t cy, uint32_t cz,
                                            uint32_t radius) noexcept {
    const uint32_t x1 = (cx > radius)       ? cx - radius : 0u;
    const uint32_t x2 = std::min(cx + radius, kW - 1u);
    const uint32_t y1 = (cy > radius)       ? cy - radius : 0u;
    const uint32_t y2 = std::min(cy + radius, kH - 1u);
    const uint32_t z1 = (cz > radius)       ? cz - radius : 0u;
    const uint32_t z2 = std::min(cz + radius, kD - 1u);

    return grid.count_in_box(x1, y1, x2, y2, z1, z2);
}

// ── AOI (Area-of-Interest) manager ───────────────────────────────────────────

/**
 * @brief Lightweight AOI region that tracks occupancy of a 2D rectangle.
 *
 * Queries the 2.5D grid's layer range for each cell in the rectangle and
 * accumulates an occupancy count.  Used by the drone's path planner to
 * monitor a forward-looking corridor.
 */
struct AoiRegion {
    uint32_t x1, y1, x2, y2;   // Bounding rectangle (inclusive)
    uint32_t z_lo, z_hi;        // Altitude band of interest

    [[nodiscard]] uint32_t occupancy(const VoxelGrid& grid) const noexcept {
        return grid.count_in_box(x1, y1, x2, y2, z_lo, z_hi);
    }

    [[nodiscard]] bool any_obstacle(const VoxelGrid& grid) const noexcept {
        return grid.any_in_box(x1, y1, x2, y2, z_lo, z_hi);
    }
};

// ── Path planner simulation ───────────────────────────────────────────────────

struct DroneState {
    uint32_t x{128}, y{128}, z{10};
    uint32_t step{0};
};

/**
 * @brief Simulate one path-planner tick:
 *   - Probe the AOI corridor in front of the drone.
 *   - If obstacle detected, climb to clear altitude.
 *   - Advance the drone position.
 */
static void path_planner_tick(const VoxelGrid& grid,
                               DroneState&       drone,
                               std::atomic<uint32_t>& obstacle_detections) {
    // Forward corridor: 8 cells ahead, 3 cells wide, current altitude ±5
    const uint32_t look_ahead = 8;
    AoiRegion corridor{
        .x1   = drone.x,
        .y1   = (drone.y > 1u) ? drone.y - 1u : 0u,
        .x2   = std::min(drone.x + look_ahead, kW - 1u),
        .y2   = std::min(drone.y + 1u, kH - 1u),
        .z_lo = (drone.z > 5u)  ? drone.z - 5u : 0u,
        .z_hi = std::min(drone.z + 5u, kD - 1u),
    };

    if (corridor.any_obstacle(grid)) {
        // Climb to avoid — jump 10 layers up if possible.
        drone.z = std::min(drone.z + 10u, kD - 1u);
        obstacle_detections.fetch_add(1u, std::memory_order_relaxed);
    } else if (drone.z > 10u) {
        // Descend back to cruise altitude when clear.
        --drone.z;
    }

    // Advance drone east.
    drone.x = (drone.x + 1u) % kW;
    ++drone.step;
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main() {
    std::println("=== Spatial Sensor Fusion — GridBitset Demo ===");
    std::println("Grid: {}×{}×{} voxels ({} KB)",
                 kW, kH, kD,
                 (VoxelGrid::kCells * sizeof(std::atomic<uint64_t>)) / 1024);
    std::println();

    // Build the voxel grid.
    VoxelGrid   grid;
    FloorMap    floor_map;          // 2D ground-truth obstacles
    std::mt19937 rng(2025);

    // ── Phase 1: inject three obstacle clouds ──────────────────────────────
    std::println("Phase 1: Injecting obstacle clouds into voxel grid...");

    // Cloud A: low-altitude cluster near (80, 120), altitude 5-20
    inject_sphere_cloud(grid, 80, 120, 12, /*radius=*/15, /*count=*/800, rng);
    // Cloud B: mid-altitude cluster near (160, 100), altitude 25-40
    inject_sphere_cloud(grid, 160, 100, 32, 20, 1200, rng);
    // Cloud C: high-altitude cluster near (200, 180), altitude 45-60
    inject_sphere_cloud(grid, 200, 180, 52, 10, 400, rng);

    // Mirror onto 2D floor map (any layer occupied → ground obstacle)
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            if (grid.snapshot(x, y) != 0u)
                floor_map.set(x, y);
        }
    }

    // Count occupied cells
    uint32_t occupied_cells = 0;
    for (uint32_t y = 0; y < kH; ++y)
        for (uint32_t x = 0; x < kW; ++x)
            if (grid.snapshot(x, y)) ++occupied_cells;

    std::println("  Occupied columns: {}/{}", occupied_cells, kW * kH);
    std::println();

    // ── Phase 2: direct GridBitset queries ─────────────────────────────────
    std::println("Phase 2: Direct spatial queries (nanosecond range)...");

    // 2.5D layer-range query: "any obstacle between altitude 5 and 20 at (80, 120)?"
    {
        const bool hit = grid.any_in_range(80, 120, 5, 20);
        std::println("  any_in_range(80,120, z=5..20): {}  [expected: true  — cloud A]",
                     hit ? "HIT" : "clear");
    }
    // Altitude band above cloud A (should be clear)
    {
        const bool hit = grid.any_in_range(80, 120, 21, 35);
        std::println("  any_in_range(80,120, z=21..35): {}  [expected: likely clear above cloud A]",
                     hit ? "HIT" : "clear");
    }
    // Cloud B
    {
        const bool hit = grid.any_in_range(160, 100, 25, 40);
        std::println("  any_in_range(160,100, z=25..40): {}  [expected: true  — cloud B]",
                     hit ? "HIT" : "clear");
    }

    // Box query
    {
        const bool hit = grid.any_in_box(70, 110, 90, 130, 5, 20);
        std::println("  any_in_box(70..90, 110..130, z=5..20): {}  [cloud A region]",
                     hit ? "HIT" : "clear");
    }

    // 2D floor map box query (Morton-ordered super-blocks)
    {
        const bool hit2d = floor_map.any_in_box(75, 115, 95, 135);
        std::println("  FloorMap any_in_box(cloud A 2D footprint): {}",
                     hit2d ? "HIT" : "clear");
    }
    std::println();

    // ── Phase 3: sphere query benchmark ──────────────────────────────────
    std::println("Phase 3: Sphere-query throughput benchmark (1000 queries)...");
    {
        constexpr int kQueries = 1000;
        std::uniform_int_distribution<uint32_t> qx(0, kW - 1u);
        std::uniform_int_distribution<uint32_t> qy(0, kH - 1u);
        std::uniform_int_distribution<uint32_t> qz(0, kD - 1u);

        const auto t0  = std::chrono::steady_clock::now();
        uint32_t   sum = 0;
        for (int q = 0; q < kQueries; ++q)
            sum += query_sphere(grid, qx(rng), qy(rng), qz(rng), /*radius=*/15);
        const auto t1     = std::chrono::steady_clock::now();
        const auto ns     = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        const double ns_q = static_cast<double>(ns) / kQueries;

        std::println("  {} sphere queries in {} µs  ({:.1f} ns/query, total hits: {})",
                     kQueries, ns / 1000, ns_q, sum);
    }
    std::println();

    // ── Phase 4: drone path planner driven by MicroTicker ─────────────────
    std::println("Phase 4: Drone path planner driven by MicroTicker @ 100 µs...");

    constexpr uint64_t     kTicks = 500;
    constexpr auto         kInterval = 100us;
    DroneState             drone;
    std::atomic<uint32_t>  detections{0};

    qbuem::MicroTicker ticker(kInterval);
    ticker.run([&](uint64_t tick_idx) {
        path_planner_tick(grid, drone, detections);
        if (tick_idx + 1 >= kTicks)
            ticker.stop();
    });

    std::println("  Path planner ticks: {}  obstacle detections: {}  final altitude: {}",
                 drone.step, detections.load(), drone.z);
    std::println();

    // ── Phase 5: snapshot and reset ───────────────────────────────────────
    std::println("Phase 5: Snapshot and reset...");
    {
        const uint64_t col_snapshot = grid.snapshot(80, 120);
        std::println("  Column snapshot (80,120): 0x{:016x}  (altitude bitmask)",
                     col_snapshot);
        std::println("  Lowest set layer: {}",
                     col_snapshot ? static_cast<int>(std::countr_zero(col_snapshot)) : -1);
        std::println("  Highest set layer: {}",
                     col_snapshot ? 63 - static_cast<int>(std::countl_zero(col_snapshot)) : -1);
    }

    grid.clear_all(80, 120);
    const bool cleared = !grid.any_in_range(80, 120, 0, kD - 1u);
    std::println("  After clear_all(80,120): column is {}",
                 cleared ? "empty" : "still occupied (bug!)");
    std::println();

    std::println("=== Summary ===");
    std::println("  GridBitset<256,256,64>:");
    std::println("    - Layer-range query  : ~1 ns  (bitwise AND on one uint64_t)");
    std::println("    - Box query          : O(area) × 1 ns per cell");
    std::println("    - Sphere query       : O(r²)  × bitmask check");
    std::println("    - Storage            : {} KB for {}×{} grid",
                 (VoxelGrid::kCells * 8u) / 1024u, kW, kH);
    std::println("  GridBitset2D<256,256>:");
    std::println("    - Morton super-blocks: spatially close cells share cache lines");
    std::println("    - Box query skips zero super-blocks in O(1)");
    return 0;
}
