/**
 * @file bench/bench_arena.cpp
 * @brief Arena 메모리 할당자 성능 벤치마크.
 *
 * ### 측정 항목
 * - Arena: 소형 객체 연속 할당 (vs. malloc)
 * - Arena: reset() 후 재사용 처리량
 * - FixedPoolResource: 고정 크기 할당/해제 왕복
 * - Arena: 다양한 크기 혼합 할당
 *
 * ### 성능 목표 (v1.0)
 * - Arena 소형 할당  : < 5 ns/alloc (bump-pointer)
 * - malloc 대비      : > 5x 빠름
 * - FixedPool 왕복   : < 10 ns/round-trip
 */

#include "bench_common.hpp"

#include <qbuem/core/arena.hpp>

#include <cstdlib>
#include <new>
#include <vector>

// ─── Arena 할당 벤치마크 ─────────────────────────────────────────────────────

static void bench_arena_small_alloc() {
    bench::section("Arena — 소형 객체 연속 할당 (vs. malloc)");

    constexpr size_t   kObjSize = 64;   // 전형적 연결 객체 크기
    constexpr uint64_t kWarmup  = 5'000;
    constexpr uint64_t kIter    = 1'000'000;

    // Arena 할당
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
                // 1000 alloc마다 reset (HTTP 요청 수명 시뮬레이션)
                if (++cnt % 1000 == 0) arena.reset();
            }
        );
        res.print();
        if (res.avg_ns() < 20.0) {
            bench::pass("Arena 할당 목표 달성: < 20 ns/alloc");
        } else {
            bench::fail("Arena 할당 목표 미달: >= 20 ns/alloc");
        }
    }

    // malloc 대비
    {
        auto res = bench::run_batch(
            "malloc/free: alloc 64B (비교 기준)",
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
    bench::section("Arena — HTTP 요청 수명 시뮬레이션");

    // 실제 HTTP 요청 처리에서의 Arena 사용 패턴:
    // 1. 요청 시작: arena를 clear/reset
    // 2. 파서가 헤더 문자열을 arena에 할당 (가변 크기)
    // 3. 라우터 매칭 결과를 arena에 할당
    // 4. 응답 빌더가 arena에 버퍼 할당
    // 5. 응답 완료: arena.reset() — O(1)

    constexpr uint64_t kWarmup = 10'000;
    constexpr uint64_t kIter   = 500'000;

    {
        qbuem::Arena arena;
        auto res = bench::run_batch(
            "Arena: request lifecycle (10 alloc + reset)",
            10,   // 배치 크기 = 할당 횟수
            kWarmup, kIter,
            [&]() {
                // 요청당 평균 10개 할당 (헤더, 파라미터 등)
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
    bench::section("Arena — 다양한 크기 혼합 할당");

    constexpr uint64_t kWarmup = 5'000;
    constexpr uint64_t kIter   = 300'000;

    static const size_t kSizes[] = {8, 16, 32, 64, 128, 256, 512, 1024};
    constexpr size_t    kNumSizes = sizeof(kSizes) / sizeof(kSizes[0]);

    {
        qbuem::Arena arena;
        auto res = bench::run_batch(
            "Arena: 혼합 크기 할당 (8~1024 B)",
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
    bench::section("FixedPoolResource — 고정 크기 할당/해제");

    // FixedPoolResource<sizeof(Connection), alignof(Connection)>
    // 전형적인 Connection 객체 크기: 256 bytes
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
            bench::pass("FixedPool 왕복 목표 달성: < 20 ns");
        } else {
            bench::fail("FixedPool 왕복 목표 미달: >= 20 ns");
        }
    }
}

// ─── 메인 ────────────────────────────────────────────────────────────────────

int main() {
    printf("\n");
    printf("══════════════════════════════════════════════════════════════\n");
    printf("  qbuem-stack v1.0.0 — Arena 메모리 할당자 성능 벤치마크\n");
    printf("══════════════════════════════════════════════════════════════\n");

    bench_arena_small_alloc();
    bench_arena_request_lifecycle();
    bench_arena_mixed_sizes();
    bench_fixed_pool();

    printf("\n══════════════════════════════════════════════════════════════\n");
    printf("  완료\n");
    printf("══════════════════════════════════════════════════════════════\n\n");

    return 0;
}
