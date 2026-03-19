/**
 * @file bench/bench_pipeline.cpp
 * @brief Pipeline core component performance benchmark.
 *
 * ### Measured Items
 * - Context::get<T>() type-key lookup latency
 * - Context::put<T>() immutable derivation cost
 * - ServiceRegistry::get<T>() resolve latency
 * - IOSlice / IOVec<N> zero-alloc buffer operations
 *
 * ### Performance Goals (v1.0)
 * - Context::get<T>()        : < 100 ns
 * - ServiceRegistry::get<T>(): < 100 ns
 * - IOSlice create + convert : < 10 ns
 */

#include "bench_common.hpp"

#include <qbuem/pipeline/context.hpp>
#include <qbuem/pipeline/service_registry.hpp>
#include <qbuem/io/io_slice.hpp>
#include <qbuem/io/iovec.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <qbuem/compat/print.hpp>
#include <string>

using namespace qbuem;

// ─── User-defined types (context.hpp already has RequestId, AuthSubject, etc.) ──

struct UserId    { uint64_t value = 0; };
struct SessionId { std::string value; };

// ─── Context benchmark ────────────────────────────────────────────────────────

static void bench_context() {
    bench::section("Context — Type-Key K/V Lookup and Derivation");

    constexpr uint64_t kWarmup = 50'000;
    constexpr uint64_t kIter   = 5'000'000;

    // Typical HTTP request context setup (using built-in types)
    Context ctx;
    ctx = ctx.put(RequestId{"req-abc123-def456"});
    ctx = ctx.put(AuthSubject{"user@example.com"});
    ctx = ctx.put(UserId{42});
    ctx = ctx.put(SessionId{"sess-xyz"});

    // get<T> hit — item closest to head
    {
        auto res = bench::run(
            "Context::get<SessionId>() (near head, 4 items)",
            kWarmup, kIter,
            [&]() {
                auto v = ctx.get<SessionId>();
                bench::do_not_optimize(v);
            }
        );
        res.print();
    }

    // get<T> hit — tail item (worst case)
    {
        auto res = bench::run(
            "Context::get<RequestId>() (tail, worst case)",
            kWarmup, kIter,
            [&]() {
                auto v = ctx.get<RequestId>();
                bench::do_not_optimize(v);
            }
        );
        res.print();
        if (res.avg_ns() < 100.0) {
            bench::pass("Context::get goal met: < 100 ns");
        } else {
            bench::fail("Context::get goal missed: >= 100 ns");
        }
    }

    // miss case
    {
        struct Absent {};
        auto res = bench::run(
            "Context::get<Absent>() (miss — full scan)",
            kWarmup, kIter,
            [&]() {
                auto v = ctx.get<Absent>();
                bench::do_not_optimize(v);
            }
        );
        res.print();
    }

    // put — derived context creation cost
    {
        auto res = bench::run(
            "Context::put<UserId>() (derived context create)",
            kWarmup / 10, kIter / 10,
            [&]() {
                auto ctx2 = ctx.put(UserId{999});
                bench::do_not_optimize(ctx2);
            }
        );
        res.print();
    }
}

// ─── ServiceRegistry benchmark ────────────────────────────────────────────────

struct ILogger {
    virtual ~ILogger() = default;
    virtual void log(std::string_view msg) = 0;
};

struct NoopLogger : ILogger {
    std::atomic<uint64_t> calls{0};
    void log(std::string_view) override {
        calls.fetch_add(1, std::memory_order_relaxed);
    }
};

struct IMetrics {
    virtual ~IMetrics() = default;
    virtual void inc(std::string_view) = 0;
};

struct NoopMetrics : IMetrics {
    std::atomic<uint64_t> count{0};
    void inc(std::string_view) override {
        count.fetch_add(1, std::memory_order_relaxed);
    }
};

static void bench_service_registry() {
    bench::section("ServiceRegistry — Dependency Injection Resolve");

    constexpr uint64_t kWarmup = 50'000;
    constexpr uint64_t kIter   = 5'000'000;

    ServiceRegistry registry;
    registry.register_singleton<ILogger>(std::make_shared<NoopLogger>());
    registry.register_singleton<IMetrics>(std::make_shared<NoopMetrics>());

    {
        auto res = bench::run(
            "ServiceRegistry::get<ILogger>()",
            kWarmup, kIter,
            [&]() {
                auto svc = registry.get<ILogger>();
                bench::do_not_optimize(svc);
            }
        );
        res.print();
        if (res.avg_ns() < 100.0) {
            bench::pass("ServiceRegistry::get goal met: < 100 ns");
        } else {
            bench::fail("ServiceRegistry::get goal missed: >= 100 ns");
        }
    }

    {
        auto res = bench::run(
            "ServiceRegistry::get<IMetrics>()",
            kWarmup, kIter,
            [&]() {
                auto svc = registry.get<IMetrics>();
                bench::do_not_optimize(svc);
            }
        );
        res.print();
    }
}

// ─── IOSlice / IOVec<N> benchmark ─────────────────────────────────────────────

static void bench_ioslice() {
    bench::section("IOSlice / IOVec<N> — Zero-Alloc Buffer Operations");

    // Use run_batch with large batch to avoid clock_gettime overhead swamping
    // sub-5 ns operations (each clock_gettime itself takes ~15 ns).
    constexpr uint64_t kBatch  = 1000;
    constexpr uint64_t kWarmup = 100;
    constexpr uint64_t kRuns   = 10'000;

    alignas(64) std::array<std::byte, 4096> buf{};

    // IOSlice create + iovec conversion
    {
        auto res = bench::run_batch(
            "IOSlice: create + to_iovec()",
            kBatch, kWarmup, kRuns,
            [&]() {
                for (uint64_t i = 0; i < kBatch; ++i) {
                    IOSlice slice{buf.data(), buf.size()};
                    auto iov = slice.to_iovec();
                    bench::do_not_optimize(iov);
                }
            }
        );
        res.print();
        if (res.avg_ns() < 10.0) {
            bench::pass("IOSlice goal met: < 10 ns");
        } else {
            bench::fail("IOSlice goal missed: >= 10 ns");
        }
    }

    // IOSlice + BufferView conversion
    {
        auto res = bench::run_batch(
            "IOSlice: create + to_buffer_view()",
            kBatch, kWarmup, kRuns,
            [&]() {
                for (uint64_t i = 0; i < kBatch; ++i) {
                    IOSlice slice{buf.data(), buf.size()};
                    auto bv = slice.to_buffer_view();
                    bench::do_not_optimize(bv);
                }
            }
        );
        res.print();
    }

    // MutableIOSlice create + conversion
    {
        auto res = bench::run_batch(
            "MutableIOSlice: create + to_iovec()",
            kBatch, kWarmup, kRuns,
            [&]() {
                for (uint64_t i = 0; i < kBatch; ++i) {
                    MutableIOSlice mslice{buf.data(), buf.size()};
                    auto iov = mslice.to_iovec();
                    bench::do_not_optimize(iov);
                }
            }
        );
        res.print();
    }

    // IOVec<4> scatter-gather construction
    {
        alignas(64) std::array<std::byte, 256>  h1{}, h2{};
        alignas(64) std::array<std::byte, 2048> body{};
        alignas(64) std::array<std::byte, 64>   trail{};

        auto res = bench::run_batch(
            "IOVec<4>: 4x push (writev scatter-gather)",
            kBatch, kWarmup, kRuns,
            [&]() {
                for (uint64_t i = 0; i < kBatch; ++i) {
                    IOVec<4> iov;
                    iov.push(h1.data(),    h1.size());
                    iov.push(h2.data(),    h2.size());
                    iov.push(body.data(),  body.size());
                    iov.push(trail.data(), trail.size());
                    bench::do_not_optimize(iov);
                }
            }
        );
        res.print();
        if (res.avg_ns() < 10.0) {
            bench::pass("IOVec<4> push goal met: < 10 ns");
        } else {
            bench::fail("IOVec<4> push goal missed: >= 10 ns");
        }
    }
}

// ─── Summary ──────────────────────────────────────────────────────────────────

static void print_summary() {
    bench::section("v1.0 Performance Goal Summary");
    std::print("  {:<45}  {}\n", "Item", "Goal");
    std::print("  {:<45}  {}\n",
           "─────────────────────────────────────────────", "──────────");
    std::print("  {:<45}  {}\n", "HTTP parser throughput (GET)",    "> 300 MB/s");
    std::print("  {:<45}  {}\n", "Router static lookup",            "< 200 ns");
    std::print("  {:<45}  {}\n", "Router param lookup",             "< 300 ns");
    std::print("  {:<45}  {}\n", "Arena::allocate (bump-pointer)",  "< 20 ns");
    std::print("  {:<45}  {}\n", "FixedPool::allocate/deallocate",  "< 20 ns");
    std::print("  {:<45}  {}\n", "AsyncChannel try_send+try_recv (MPMC)",  "> 40M ops/s");
    std::print("  {:<45}  {}\n", "SpscChannel try_send+try_recv (SPSC)",   "> 100M ops/s");
    std::print("  {:<45}  {}\n", "Context::get<T>()",               "< 100 ns");
    std::print("  {:<45}  {}\n", "ServiceRegistry::get<T>()",       "< 100 ns");
    std::print("  {:<45}  {}\n", "IOSlice create+convert",          "< 10 ns");
    std::print("  {:<45}  {}\n", "IOVec<4> push x4",                "< 10 ns");
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    std::println();
    std::println("══════════════════════════════════════════════════════════════");
    std::println("  qbuem-stack — Pipeline & IO Performance Benchmark");
    std::println("══════════════════════════════════════════════════════════════");

    bench_context();
    bench_service_registry();
    bench_ioslice();
    print_summary();

    std::println();
    std::println("══════════════════════════════════════════════════════════════");
    std::println("  Done");
    std::println("══════════════════════════════════════════════════════════════");
    std::println();

    return 0;
}
