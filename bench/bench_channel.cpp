/**
 * @file bench/bench_channel.cpp
 * @brief AsyncChannel MPMC 성능 벤치마크.
 *
 * ### 측정 항목
 * - try_send/try_recv 처리량 (wait-free, single-threaded)
 * - ArenaChannel 처리량
 * - SpscChannel 처리량 (wait-free)
 * - 배치 채널 fill/drain 처리량
 *
 * ### 성능 목표 (v1.0)
 * - try_send + try_recv : > 50M ops/s (단일 스레드, 포화 없음)
 * - SpscChannel         : > 100M ops/s (wait-free SPSC)
 */

#include "bench_common.hpp"

#include <qbuem/pipeline/async_channel.hpp>
#include <qbuem/pipeline/arena_channel.hpp>
#include <qbuem/pipeline/spsc_channel.hpp>

#include <cstdint>

using namespace qbuem;

// ─── AsyncChannel try_send/try_recv (non-blocking, single-threaded) ──────────

static void bench_async_channel_trysend() {
    bench::section("AsyncChannel<int> — try_send + try_recv (non-blocking)");

    constexpr size_t   kCapacity = 4096;
    constexpr uint64_t kWarmup   = 100'000;
    constexpr uint64_t kIter     = 10'000'000;

    AsyncChannel<int> chan(kCapacity);

    // fill half
    for (size_t i = 0; i < kCapacity / 2; ++i) {
        chan.try_send(static_cast<int>(i));
    }

    {
        auto res = bench::run_batch(
            "AsyncChannel: try_send + try_recv (round-trip)",
            1,
            kWarmup, kIter,
            [&]() {
                bool sent = chan.try_send(42);
                bench::do_not_optimize(sent);
                auto v = chan.try_recv();
                bench::do_not_optimize(v);
            }
        );
        res.print();

        const double ops_s = res.ops_per_sec();
        if (ops_s >= 50e6) {
            bench::pass("처리량 목표 달성: >= 50M ops/s");
        } else {
            bench::fail("처리량 목표 미달: < 50M ops/s");
        }
    }

    // send-only burst
    {
        AsyncChannel<int> ch2(kCapacity);
        auto res = bench::run_batch(
            "AsyncChannel: try_send burst (until full)",
            kCapacity,
            10,
            100,
            [&]() {
                for (size_t i = 0; i < kCapacity; ++i) {
                    ch2.try_send(static_cast<int>(i));
                }
                // drain
                while (ch2.try_recv().has_value()) {}
            }
        );
        res.print_throughput(sizeof(int));
    }
}

static void bench_spsc_channel() {
    bench::section("SpscChannel<uint64_t> — wait-free SPSC");

    constexpr size_t   kCapacity = 4096;
    constexpr uint64_t kWarmup   = 100'000;
    constexpr uint64_t kIter     = 20'000'000;

    SpscChannel<uint64_t> chan(kCapacity);

    // pre-fill half
    for (size_t i = 0; i < kCapacity / 2; ++i) {
        chan.try_send(i);
    }

    {
        auto res = bench::run_batch(
            "SpscChannel: try_send + try_recv (round-trip)",
            1,
            kWarmup, kIter,
            [&]() {
                bool ok = chan.try_send(0xDEADBEEFULL);
                bench::do_not_optimize(ok);
                auto v = chan.try_recv();
                bench::do_not_optimize(v);
            }
        );
        res.print();

        const double ops_s = res.ops_per_sec();
        if (ops_s >= 50e6) {
            bench::pass("SpscChannel 목표 달성: >= 50M ops/s");
        } else {
            bench::fail("SpscChannel 목표 미달: < 50M ops/s");
        }
    }
}

static void bench_arena_channel() {
    bench::section("ArenaChannel<int> — Arena 기반 고속 채널");

    constexpr size_t   kCapacity = 4096;
    constexpr uint64_t kWarmup   = 50'000;
    constexpr uint64_t kIter     = 5'000'000;

    {
        ArenaChannel<int> chan(kCapacity);

        // Pre-fill half
        for (size_t i = 0; i < kCapacity / 2; ++i) {
            chan.push(static_cast<int>(i));
        }

        auto res = bench::run_batch(
            "ArenaChannel: push + pop (round-trip)",
            1,
            kWarmup, kIter,
            [&]() {
                bool ok = chan.push(99);
                bench::do_not_optimize(ok);
                auto v = chan.pop();
                bench::do_not_optimize(v);
            }
        );
        res.print();
    }
}

static void bench_channel_batch_throughput() {
    bench::section("채널 배치 처리량 비교 (fill → drain)");

    constexpr size_t   kCapacity = 8192;
    constexpr size_t   kBatch    = 1000;
    constexpr uint64_t kWarmup   = 100;
    constexpr uint64_t kRuns     = 10'000;

    // AsyncChannel batch
    {
        AsyncChannel<int> chan(kCapacity);
        auto res = bench::run_batch(
            "AsyncChannel: fill 1000 + drain 1000",
            kBatch * 2,
            kWarmup, kRuns,
            [&]() {
                for (size_t i = 0; i < kBatch; ++i) {
                    chan.try_send(static_cast<int>(i));
                }
                for (size_t i = 0; i < kBatch; ++i) {
                    auto v = chan.try_recv();
                    bench::do_not_optimize(v);
                }
            }
        );
        res.print_throughput(sizeof(int));
    }

    // SpscChannel batch
    {
        SpscChannel<int> chan(kCapacity);
        auto res = bench::run_batch(
            "SpscChannel: fill 1000 + drain 1000",
            kBatch * 2,
            kWarmup, kRuns,
            [&]() {
                for (size_t i = 0; i < kBatch; ++i) {
                    chan.try_send(static_cast<int>(i));
                }
                for (size_t i = 0; i < kBatch; ++i) {
                    auto v = chan.try_recv();
                    bench::do_not_optimize(v);
                }
            }
        );
        res.print_throughput(sizeof(int));
    }
}

// ─── 메인 ────────────────────────────────────────────────────────────────────

int main() {
    printf("\n");
    printf("══════════════════════════════════════════════════════════════\n");
    printf("  qbuem-stack v1.0.0 — 채널 성능 벤치마크\n");
    printf("══════════════════════════════════════════════════════════════\n");

    bench_async_channel_trysend();
    bench_spsc_channel();
    bench_arena_channel();
    bench_channel_batch_throughput();

    printf("\n══════════════════════════════════════════════════════════════\n");
    printf("  완료\n");
    printf("══════════════════════════════════════════════════════════════\n\n");

    return 0;
}
