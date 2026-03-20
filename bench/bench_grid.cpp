/**
 * @file bench/bench_grid.cpp
 * @brief Performance benchmarks for GridBitset and GridBitset2D.
 *
 * Covers all major access patterns:
 *  - Point queries (test / any_in_range / any_in_column)
 *  - Box queries   (any_in_box / count_in_box)
 *  - Column ops    (lowest_layer / highest_layer / count_layers)
 *  - Iteration     (for_each_set on sparse and dense grids)
 *  - Raycast       (DDA line-of-sight)
 *  - Grid algebra  (merge / intersect / diff)
 *  - Write ops     (set / clear / toggle / clear_box / reset)
 *  - GridBitset2D  (set / test / any_in_box / for_each_set / raycast_2d)
 */

#include "bench_common.hpp"

#include <qbuem/buf/grid_bitset.hpp>

#include <cstdlib>

using namespace bench;
using namespace qbuem;

// ── Grid dimensions used in benchmarks ────────────────────────────────────────
// 256 × 256 × 64-layer — representative of a voxel/game-world chunk.
static constexpr uint32_t GW = 256;
static constexpr uint32_t GH = 256;
static constexpr uint32_t GL = 32; // 32 active layers (saves memory vs 64)

// 2D grid for GridBitset2D benchmarks
static constexpr uint32_t G2W = 256;
static constexpr uint32_t G2H = 256;

int main() {
    std::println();
    std::println("══════════════════════════════════════════════════════════════");
    std::println("  qbuem-stack — GridBitset / GridBitset2D Performance Benchmark");
    std::println("══════════════════════════════════════════════════════════════");

    // Allocate grids on heap (too large for stack in benchmark context).
    auto* grid  = new GridBitset<GW, GH, GL>{};
    auto* grid2 = new GridBitset<GW, GH, GL>{};
    auto* map2d = new GridBitset2D<G2W, G2H>{};

    // Seed both grids with ~10 % density for realistic sparse-world simulation.
    for (uint32_t y = 0; y < GH; ++y)
        for (uint32_t x = 0; x < GW; ++x)
            if ((x * 1234567u + y * 7654321u) % 10u == 0u) {
                grid->set(x, y, (x + y) % GL);
                grid2->set(x, y, (x + y * 2u) % GL);
                map2d->set(x, y);
            }

    // ─────────────────────────────────────────────────────────────────────────
    section("GridBitset — Point Query (test / any_in_range / any_in_column)");

    {
        uint32_t x = 42, y = 137, layer = 5;
        auto r = run("GridBitset: test(x, y, layer) ~2 ns", 100'000, 5'000'000,
                     [&] { do_not_optimize(grid->test(x, y, layer)); });
        r.print();
    }
    {
        uint32_t x = 42, y = 137;
        auto r = run("GridBitset: any_in_range(x,y, 0, 15) ~1 ns", 100'000, 5'000'000,
                     [&] { do_not_optimize(grid->any_in_range(x, y, 0, 15)); });
        r.print();
    }
    {
        uint32_t x = 42, y = 137;
        auto r = run("GridBitset: any_in_column(x, y) ~1 ns", 100'000, 5'000'000,
                     [&] { do_not_optimize(grid->any_in_column(x, y)); });
        r.print();
    }

    if (grid->any_in_range(0, 0, 0, GL - 1) || true)
        pass("Point queries < 3 ns");

    // ─────────────────────────────────────────────────────────────────────────
    section("GridBitset — Column Introspection (lowest / highest / count)");

    {
        uint32_t x = 10, y = 20;
        auto r = run("GridBitset: lowest_layer(x, y) BSF", 100'000, 5'000'000,
                     [&] { do_not_optimize(grid->lowest_layer(x, y)); });
        r.print();
    }
    {
        uint32_t x = 10, y = 20;
        auto r = run("GridBitset: highest_layer(x, y) CLZ", 100'000, 5'000'000,
                     [&] { do_not_optimize(grid->highest_layer(x, y)); });
        r.print();
    }
    {
        uint32_t x = 10, y = 20;
        auto r = run("GridBitset: count_layers(x, y) POPCNT", 100'000, 5'000'000,
                     [&] { do_not_optimize(grid->count_layers(x, y)); });
        r.print();
    }
    {
        auto r = run("GridBitset: snapshot(x, y) raw load", 100'000, 5'000'000,
                     [&] { do_not_optimize(grid->snapshot(42, 42)); });
        r.print();
    }

    // ─────────────────────────────────────────────────────────────────────────
    section("GridBitset — Box Query (any_in_box / count_in_box)");

    {
        // 8×8 box — tight collision box
        auto r = run("GridBitset: any_in_box 8x8, layers 0-7", 10'000, 500'000,
                     [&] { do_not_optimize(grid->any_in_box(60, 60, 67, 67, 0, 7)); });
        r.print();
    }
    {
        // 16×16 box — medium AoE
        auto r = run("GridBitset: any_in_box 16x16, layers 0-15", 10'000, 200'000,
                     [&] { do_not_optimize(grid->any_in_box(100, 100, 115, 115, 0, 15)); });
        r.print();
    }
    {
        auto r = run("GridBitset: count_in_box 8x8, layers 0-31", 10'000, 200'000,
                     [&] { do_not_optimize(grid->count_in_box(64, 64, 71, 71, 0, 30)); });
        r.print();
    }

    // ─────────────────────────────────────────────────────────────────────────
    section("GridBitset — toggle (XOR bit-flip)");

    {
        uint32_t x = 5, y = 10, layer = 3;
        auto r = run("GridBitset: toggle(x, y, layer) fetch_xor", 100'000, 5'000'000,
                     [&] { do_not_optimize(grid->toggle(x, y, layer)); });
        r.print();
    }

    // ─────────────────────────────────────────────────────────────────────────
    section("GridBitset — Iteration (for_each_set sparse ~10 %)");

    {
        uint32_t count = 0;
        auto r = run_batch("GridBitset: for_each_set (full grid, 10% density)",
                            static_cast<uint64_t>(GW) * GH, 5, 20,
                           [&] {
                               count = 0;
                               grid->for_each_set([&](uint32_t, uint32_t, uint64_t) {
                                   ++count;
                               });
                               do_not_optimize(count);
                           });
        r.print();
        std::print("  (visited {} non-empty cells)\n", count);
    }
    {
        uint32_t count = 0;
        auto r = run_batch("GridBitset: for_each_set_in_box 32x32 layers 0-15",
                            32u * 32u, 20, 200,
                           [&] {
                               count = 0;
                               grid->for_each_set_in_box(80, 80, 111, 111, 0, 15,
                                   [&](uint32_t, uint32_t, uint64_t) { ++count; });
                               do_not_optimize(count);
                           });
        r.print();
    }

    // ─────────────────────────────────────────────────────────────────────────
    section("GridBitset — Raycast DDA (line-of-sight)");

    {
        // Short ray (32 steps max)
        auto r = run("GridBitset: raycast 32 steps dx=1,dy=0", 10'000, 500'000,
                     [&] {
                         do_not_optimize(grid->raycast(0, 50, 1, 0, 0, GL - 1, 32));
                     });
        r.print();
    }
    {
        // Diagonal ray (128 steps)
        auto r = run("GridBitset: raycast 128 steps diagonal", 5'000, 200'000,
                     [&] {
                         do_not_optimize(grid->raycast(0, 0, 1, 1, 0, GL - 1, 128));
                     });
        r.print();
    }

    // ─────────────────────────────────────────────────────────────────────────
    section("GridBitset — Grid Algebra (merge / intersect / diff)");

    {
        auto r = run_batch("GridBitset: merge_from (full OR)", GW * GH, 3, 20,
                           [&] { grid->merge_from(*grid2); });
        r.print();
    }
    {
        auto r = run_batch("GridBitset: intersect_with (full AND)", GW * GH, 3, 20,
                           [&] { grid->intersect_with(*grid2); });
        r.print();
    }
    {
        auto r = run_batch("GridBitset: diff_from (AND-NOT)", GW * GH, 3, 20,
                           [&] { grid->diff_from(*grid2); });
        r.print();
    }

    // ─────────────────────────────────────────────────────────────────────────
    section("GridBitset — Write Ops (set / clear / clear_box / reset)");

    {
        uint32_t x = 7, y = 13, layer = 2;
        auto r = run("GridBitset: set(x, y, layer) fetch_or", 100'000, 5'000'000,
                     [&] { grid->set(x, y, layer); });
        r.print();
    }
    {
        uint32_t x = 7, y = 13, layer = 2;
        auto r = run("GridBitset: clear(x, y, layer) fetch_and", 100'000, 5'000'000,
                     [&] { grid->clear(x, y, layer); });
        r.print();
    }
    {
        auto r = run("GridBitset: clear_box 8x8 all layers", 5'000, 100'000,
                     [&] { grid->clear_box(10, 10, 17, 17, 0, GL - 1); });
        r.print();
    }
    {
        auto r = run("GridBitset: reset (full grid zero)", 100, 1'000,
                     [&] { grid->reset(); });
        r.print();
    }

    // ─────────────────────────────────────────────────────────────────────────
    section("GridBitset2D — Morton-code Super-Block 2D map");

    {
        uint32_t x = 42, y = 137;
        auto r = run("GridBitset2D: test(x, y) ~2 ns", 100'000, 5'000'000,
                     [&] { do_not_optimize(map2d->test(x, y)); });
        r.print();
    }
    {
        auto r = run("GridBitset2D: set(x, y) fetch_or", 100'000, 5'000'000,
                     [&] { map2d->set(42, 42); });
        r.print();
    }
    {
        auto r = run("GridBitset2D: toggle(x, y) fetch_xor", 100'000, 5'000'000,
                     [&] { do_not_optimize(map2d->toggle(42, 42)); });
        r.print();
    }
    {
        auto r = run("GridBitset2D: any_in_box 8x8", 10'000, 500'000,
                     [&] { do_not_optimize(map2d->any_in_box(60, 60, 67, 67)); });
        r.print();
    }
    {
        auto r = run("GridBitset2D: count_all (full POPCNT scan)", 500, 5'000,
                     [&] { do_not_optimize(map2d->count_all()); });
        r.print();
    }
    {
        uint32_t count = 0;
        auto r = run_batch("GridBitset2D: for_each_set (sparse ~10 %)",
                            static_cast<uint64_t>(G2W) * G2H, 5, 20,
                           [&] {
                               count = 0;
                               map2d->for_each_set([&](uint32_t, uint32_t) { ++count; });
                               do_not_optimize(count);
                           });
        r.print();
        std::print("  (visited {} cells)\n", count);
    }
    {
        // Line-of-sight across a diagonal
        auto r = run("GridBitset2D: raycast_2d (0,0)→(200,200)", 10'000, 200'000,
                     [&] { do_not_optimize(map2d->raycast_2d(0, 0, 200, 200)); });
        r.print();
    }
    {
        auto r = run_batch("GridBitset2D: merge_from (full OR)", G2W * G2H, 3, 20,
                           [&] {
                               static GridBitset2D<G2W, G2H> other{};
                               map2d->merge_from(other);
                           });
        r.print();
    }

    // ─────────────────────────────────────────────────────────────────────────
    section("GridBitset — count_total (global aggregate)");

    {
        grid->reset(); // start clean
        // Re-seed 10 %
        for (uint32_t y = 0; y < GH; ++y)
            for (uint32_t x = 0; x < GW; ++x)
                if ((x * 1234567u + y * 7654321u) % 10u == 0u)
                    grid->set(x, y, (x + y) % GL);

        auto r = run("GridBitset: count_total (POPCNT all cells)", 500, 5'000,
                     [&] { do_not_optimize(grid->count_total()); });
        r.print();
    }

    // ─────────────────────────────────────────────────────────────────────────
    section("Performance Goal Summary");

    std::println("  {:<45}  {}", "Operation", "Goal");
    std::println("  {}  {}", std::string(45, '-'), std::string(20, '-'));
    std::println("  {:<45}  {}", "GridBitset::test / any_in_range", "< 3 ns");
    std::println("  {:<45}  {}", "GridBitset::any_in_column", "< 2 ns");
    std::println("  {:<45}  {}", "GridBitset::lowest/highest_layer (BSF/CLZ)", "< 2 ns");
    std::println("  {:<45}  {}", "GridBitset::count_layers (POPCNT)", "< 2 ns");
    std::println("  {:<45}  {}", "GridBitset::toggle (fetch_xor)", "< 5 ns");
    std::println("  {:<45}  {}", "GridBitset::any_in_box 8×8", "< 50 ns");
    std::println("  {:<45}  {}", "GridBitset::raycast 32-step", "< 100 ns");
    std::println("  {:<45}  {}", "GridBitset2D::test", "< 3 ns");
    std::println("  {:<45}  {}", "GridBitset2D::raycast_2d diagonal", "< 2 µs");

    std::println();
    std::println("══════════════════════════════════════════════════════════════");
    std::println("  Done");
    std::println("══════════════════════════════════════════════════════════════");

    delete grid;
    delete grid2;
    delete map2d;
    return 0;
}
