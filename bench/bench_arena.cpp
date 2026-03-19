/**
 * @file bench/bench_arena.cpp
 * @brief Arena memory allocator performance benchmark.
 *
 * ### Measured Items
 * - Arena: small object sequential allocation (vs. malloc)
 * - Arena: throughput after reset()
 * - FixedPoolResource: fixed-size alloc/dealloc round-trip
 * - Arena: mixed-size allocation
 *
 * ### Performance Goals (v1.0)
 * - Arena small alloc  : < 5 ns/alloc (bump-pointer)
 * - vs. malloc         : > 5x faster
 * - FixedPool round-trip: < 10 ns/round-trip
 */

#include "bench_common.hpp"

#include <qbuem/core/arena.hpp>

#include <cstdlib>
#include <new>
#include <print>
#include <vector>

// ─── Arena allocation benchmark ──────────────────────────────────────────────

static void bench_arena_small_alloc() {
    bench::section("Arena — Small Object Sequential Allocation (vs. malloc)");

    constexpr size_t   kObjSize = 64;   // typical connection object size
    constexpr uint64_t kWarmup  = 5'000;
    constexpr uint64_t kIter    = 1'000'000;

    // Arena allocation
    {
        qbuem::Arena arena;
        uint64_t cnt = 0;
        auto res = bench::run_batch(
            "Arena: alloc 64B (bump-pointer)",
            1,
            kWarmup, kIter,
            [&]() {
                void* p = arena.allocate(kObjSize, alignof(std::max_align_t));
                bench::do_not_optimize(p);
                // reset every 1000 allocs (simulates HTTP request lifetime)
                if (++cnt % 1000 == 0) arena.reset();
            }
        );
        res.print();
        if (res.avg_ns() < 20.0) {
            bench::pass("Arena alloc goal met: < 20 ns/alloc");
        } else {
            bench::fail("Arena alloc goal missed: >= 20 ns/alloc");
        }
    }

    // vs. malloc
    {
        auto res = bench::run_batch(
            "malloc/free: alloc 64B (baseline)",
            1,
            kWarmup, kIter,
            [&]() {
                void* p = std::malloc(kObjSize);
                bench::do_not_optimize(p);
                std::free(p);
            }
        );
        res.print();
    }
}

static void bench_arena_request_lifecycle() {
    bench::section("Arena — HTTP Request Lifecycle Simulation");

    // Typical Arena usage pattern in HTTP request handling:
    // 1. Request start: arena reset/clear
    // 2. Parser allocates header strings into arena (variable size)
    // 3. Router match results allocated into arena
    // 4. Response builder allocates buffer into arena
    // 5. Response complete: arena.reset() — O(1)

    constexpr uint64_t kWarmup = 10'000;
    constexpr uint64_t kIter   = 500'000;

    {
        qbuem::Arena arena;
        auto res = bench::run_batch(
            "Arena: request lifecycle (10 alloc + reset)",
            10,   // batch size = number of allocations
            kWarmup, kIter,
            [&]() {
                // ~10 allocations per request (headers, params, etc.)
                for (int i = 0; i < 10; ++i) {
                    size_t sz = 16 + static_cast<size_t>(i) * 8; // 16~88 bytes
                    void* p = arena.allocate(sz, 8);
                    bench::do_not_optimize(p);
                }
                arena.reset();
            }
        );
        res.print();
    }
}

static void bench_arena_mixed_sizes() {
    bench::section("Arena — Mixed-Size Allocation");

    constexpr uint64_t kWarmup = 5'000;
    constexpr uint64_t kIter   = 300'000;

    static const size_t kSizes[] = {8, 16, 32, 64, 128, 256, 512, 1024};
    constexpr size_t    kNumSizes = sizeof(kSizes) / sizeof(kSizes[0]);

    {
        qbuem::Arena arena;
        auto res = bench::run_batch(
            "Arena: mixed-size alloc (8~1024 B)",
            kNumSizes,
            kWarmup, kIter,
            [&]() {
                for (size_t s : kSizes) {
                    void* p = arena.allocate(s, alignof(std::max_align_t));
                    bench::do_not_optimize(p);
                }
                arena.reset();
            }
        );
        res.print();
    }
}

static void bench_fixed_pool() {
    bench::section("FixedPoolResource — Fixed-Size Alloc/Dealloc");

    // FixedPoolResource<sizeof(Connection), alignof(Connection)>
    // Typical Connection object size: 256 bytes
    constexpr size_t kObjSize   = 256;
    constexpr size_t kAlignment = 64;  // cache-line aligned
    constexpr size_t kPoolCount = 1024;

    constexpr uint64_t kBatch  = 1000;
    constexpr uint64_t kWarmup = 100;
    constexpr uint64_t kRuns   = 10'000;

    {
        qbuem::FixedPoolResource<kObjSize, kAlignment> pool(kPoolCount);

        auto res = bench::run_batch(
            "FixedPool: alloc + dealloc 256B (free-list)",
            kBatch, kWarmup, kRuns,
            [&]() {
                for (uint64_t i = 0; i < kBatch; ++i) {
                    void* p = pool.allocate();
                    bench::do_not_optimize(p);
                    if (p) {
                        pool.deallocate(p);
                    }
                }
            }
        );
        res.print();
        if (res.avg_ns() < 20.0) {
            bench::pass("FixedPool round-trip goal met: < 20 ns");
        } else {
            bench::fail("FixedPool round-trip goal missed: >= 20 ns");
        }
    }
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    std::println();
    std::println("══════════════════════════════════════════════════════════════");
    std::println("  qbuem-stack — Arena Memory Allocator Performance Benchmark");
    std::println("══════════════════════════════════════════════════════════════");

    bench_arena_small_alloc();
    bench_arena_request_lifecycle();
    bench_arena_mixed_sizes();
    bench_fixed_pool();

    std::println();
    std::println("══════════════════════════════════════════════════════════════");
    std::println("  Done");
    std::println("══════════════════════════════════════════════════════════════");
    std::println();

    return 0;
}
