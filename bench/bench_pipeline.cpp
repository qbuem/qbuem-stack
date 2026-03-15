/**
 * @file bench/bench_pipeline.cpp
 * @brief Pipeline 핵심 컴포넌트 성능 벤치마크.
 *
 * ### 측정 항목
 * - Context::get<T>() 타입 키 조회 지연
 * - Context::put<T>() 불변 파생 비용
 * - ServiceRegistry::get<T>() resolve 지연
 * - IOSlice / IOVec<N> zero-alloc 버퍼 조작
 *
 * ### 성능 목표 (v1.0)
 * - Context::get<T>()        : < 100 ns
 * - ServiceRegistry::get<T>(): < 100 ns
 * - IOSlice 생성 + 변환      : < 10 ns
 */

#include "bench_common.hpp"

#include <qbuem/pipeline/context.hpp>
#include <qbuem/pipeline/service_registry.hpp>
#include <qbuem/io/io_slice.hpp>
#include <qbuem/io/iovec.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <string>

using namespace qbuem;

// ─── 사용자 정의 타입 (context.hpp에 이미 RequestId 등 내장 타입 있음) ────────

struct UserId    { uint64_t value = 0; };
struct SessionId { std::string value; };

// ─── Context 벤치마크 ────────────────────────────────────────────────────────

static void bench_context() {
    bench::section("Context — 타입-키 K/V 조회 및 파생");

    constexpr uint64_t kWarmup = 50'000;
    constexpr uint64_t kIter   = 5'000'000;

    // 일반적인 HTTP 요청 컨텍스트 구성 (내장 타입 사용)
    Context ctx;
    ctx = ctx.put(RequestId{"req-abc123-def456"});
    ctx = ctx.put(AuthSubject{"user@example.com"});
    ctx = ctx.put(UserId{42});
    ctx = ctx.put(SessionId{"sess-xyz"});

    // get<T> 히트 — head에 가장 가까운 항목
    {
        auto res = bench::run(
            "Context::get<SessionId>() (head 근처, 4항목)",
            kWarmup, kIter,
            [&]() {
                auto v = ctx.get<SessionId>();
                bench::do_not_optimize(v);
            }
        );
        res.print();
    }

    // get<T> 히트 — 끝 쪽 항목 (최악 케이스)
    {
        auto res = bench::run(
            "Context::get<RequestId>() (tail, 최악 케이스)",
            kWarmup, kIter,
            [&]() {
                auto v = ctx.get<RequestId>();
                bench::do_not_optimize(v);
            }
        );
        res.print();
        if (res.avg_ns() < 100.0) {
            bench::pass("Context::get 목표 달성: < 100 ns");
        } else {
            bench::fail("Context::get 목표 미달: >= 100 ns");
        }
    }

    // 미스 케이스
    {
        struct Absent {};
        auto res = bench::run(
            "Context::get<Absent>() (미스 — 전체 탐색)",
            kWarmup, kIter,
            [&]() {
                auto v = ctx.get<Absent>();
                bench::do_not_optimize(v);
            }
        );
        res.print();
    }

    // put — 파생 컨텍스트 생성 비용
    {
        auto res = bench::run(
            "Context::put<UserId>() (파생 컨텍스트 생성)",
            kWarmup / 10, kIter / 10,
            [&]() {
                auto ctx2 = ctx.put(UserId{999});
                bench::do_not_optimize(ctx2);
            }
        );
        res.print();
    }
}

// ─── ServiceRegistry 벤치마크 ────────────────────────────────────────────────

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
    bench::section("ServiceRegistry — 의존성 주입 resolve");

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
            bench::pass("ServiceRegistry::get 목표 달성: < 100 ns");
        } else {
            bench::fail("ServiceRegistry::get 목표 미달: >= 100 ns");
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

// ─── IOSlice / IOVec<N> 벤치마크 ─────────────────────────────────────────────

static void bench_ioslice() {
    bench::section("IOSlice / IOVec<N> — zero-alloc 버퍼 조작");

    constexpr uint64_t kWarmup = 100'000;
    constexpr uint64_t kIter   = 10'000'000;

    alignas(64) std::array<std::byte, 4096> buf{};

    // IOSlice 생성 + iovec 변환
    {
        auto res = bench::run(
            "IOSlice: 생성 + to_iovec()",
            kWarmup, kIter,
            [&]() {
                IOSlice slice{buf.data(), buf.size()};
                auto iov = slice.to_iovec();
                bench::do_not_optimize(iov);
            }
        );
        res.print();
        if (res.avg_ns() < 10.0) {
            bench::pass("IOSlice 목표 달성: < 10 ns");
        } else {
            bench::fail("IOSlice 목표 미달: >= 10 ns");
        }
    }

    // IOSlice + BufferView 변환
    {
        auto res = bench::run(
            "IOSlice: 생성 + to_buffer_view()",
            kWarmup, kIter,
            [&]() {
                IOSlice slice{buf.data(), buf.size()};
                auto bv = slice.to_buffer_view();
                bench::do_not_optimize(bv);
            }
        );
        res.print();
    }

    // MutableIOSlice 생성 + 변환
    {
        auto res = bench::run(
            "MutableIOSlice: 생성 + to_iovec()",
            kWarmup, kIter,
            [&]() {
                MutableIOSlice mslice{buf.data(), buf.size()};
                auto iov = mslice.to_iovec();
                bench::do_not_optimize(iov);
            }
        );
        res.print();
    }

    // IOVec<4> scatter-gather 구성
    {
        alignas(64) std::array<std::byte, 256>  h1{}, h2{};
        alignas(64) std::array<std::byte, 2048> body{};
        alignas(64) std::array<std::byte, 64>   trail{};

        auto res = bench::run(
            "IOVec<4>: 4개 push (writev scatter-gather 구성)",
            kWarmup, kIter,
            [&]() {
                IOVec<4> iov;
                iov.push(h1.data(),    h1.size());
                iov.push(h2.data(),    h2.size());
                iov.push(body.data(),  body.size());
                iov.push(trail.data(), trail.size());
                bench::do_not_optimize(iov);
            }
        );
        res.print();
        if (res.avg_ns() < 10.0) {
            bench::pass("IOVec<4> push 목표 달성: < 10 ns");
        } else {
            bench::fail("IOVec<4> push 목표 미달: >= 10 ns");
        }
    }
}

// ─── 종합 요약 ────────────────────────────────────────────────────────────────

static void print_summary() {
    bench::section("v1.0 성능 목표 요약");
    printf("  %-45s  %s\n", "항목", "목표");
    printf("  %-45s  %s\n",
           "─────────────────────────────────────────────", "──────────");
    printf("  %-45s  %s\n", "HTTP 파서 처리량 (GET)",          "> 300 MB/s");
    printf("  %-45s  %s\n", "Router 정적 룩업",                "< 200 ns");
    printf("  %-45s  %s\n", "Router 파라미터 룩업",            "< 300 ns");
    printf("  %-45s  %s\n", "Arena::allocate (bump-pointer)",  "< 20 ns");
    printf("  %-45s  %s\n", "FixedPool::allocate/deallocate",  "< 20 ns");
    printf("  %-45s  %s\n", "AsyncChannel try_send+try_recv",  "> 50M ops/s");
    printf("  %-45s  %s\n", "SpscChannel try_send+try_recv",   "> 50M ops/s");
    printf("  %-45s  %s\n", "Context::get<T>()",               "< 100 ns");
    printf("  %-45s  %s\n", "ServiceRegistry::get<T>()",       "< 100 ns");
    printf("  %-45s  %s\n", "IOSlice 생성+변환",               "< 10 ns");
    printf("  %-45s  %s\n", "IOVec<4> push×4",                 "< 10 ns");
}

// ─── 메인 ────────────────────────────────────────────────────────────────────

int main() {
    printf("\n");
    printf("══════════════════════════════════════════════════════════════\n");
    printf("  qbuem-stack v1.0.0 — Pipeline & IO 성능 벤치마크\n");
    printf("══════════════════════════════════════════════════════════════\n");

    bench_context();
    bench_service_registry();
    bench_ioslice();
    print_summary();

    printf("\n══════════════════════════════════════════════════════════════\n");
    printf("  완료\n");
    printf("══════════════════════════════════════════════════════════════\n\n");

    return 0;
}
