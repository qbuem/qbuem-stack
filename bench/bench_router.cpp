/**
 * @file bench/bench_router.cpp
 * @brief Radix Tree Router 성능 벤치마크.
 *
 * ### 측정 항목
 * - 정적 경로 룩업 지연 (ns/lookup)
 * - 파라미터 경로 룩업 지연 (e.g. /user/:id)
 * - 1000개 경로 등록 후 최악 케이스 룩업
 * - 404 미스 케이스 지연
 *
 * ### 성능 목표 (v1.0)
 * - 정적 룩업  : < 200 ns
 * - 파라미터   : < 300 ns
 * - 1000-route : < 500 ns (최악)
 */

#include "bench_common.hpp"

#include <qbuem/http/request.hpp>
#include <qbuem/http/router.hpp>

#include <string>
#include <unordered_map>
#include <vector>

using qbuem::Method;
using qbuem::Router;

// ─── 더미 핸들러 ─────────────────────────────────────────────────────────────

static qbuem::Handler noop_handler = [](const qbuem::Request&, qbuem::Response&) {};

// ─── 벤치마크 ────────────────────────────────────────────────────────────────

static void bench_static_lookup() {
    bench::section("Router — 정적 경로 룩업");

    Router router;
    router.add_route(Method::Get,    "/",                     noop_handler);
    router.add_route(Method::Get,    "/api/v1/health",        noop_handler);
    router.add_route(Method::Get,    "/api/v1/users",         noop_handler);
    router.add_route(Method::Post,   "/api/v1/users",         noop_handler);
    router.add_route(Method::Get,    "/api/v1/users/:id",     noop_handler);
    router.add_route(Method::Put,    "/api/v1/users/:id",     noop_handler);
    router.add_route(Method::Delete, "/api/v1/users/:id",     noop_handler);
    router.add_route(Method::Get,    "/api/v1/products",      noop_handler);
    router.add_route(Method::Get,    "/api/v1/products/:id",  noop_handler);
    router.add_route(Method::Get,    "/metrics",              noop_handler);

    constexpr uint64_t kWarmup = 50'000;
    constexpr uint64_t kIter   = 2'000'000;

    std::unordered_map<std::string, std::string> params;

    {
        auto res = bench::run(
            "Router: GET /api/v1/health (정적)",
            kWarmup, kIter,
            [&]() {
                params.clear();
                auto h = router.match(Method::Get, "/api/v1/health", params);
                bench::do_not_optimize(h);
            }
        );
        res.print();
        if (res.avg_ns() < 200.0) {
            bench::pass("정적 룩업 목표 달성: < 200 ns");
        } else {
            bench::fail("정적 룩업 목표 미달: >= 200 ns");
        }
    }

    {
        auto res = bench::run(
            "Router: GET / (루트)",
            kWarmup, kIter,
            [&]() {
                params.clear();
                auto h = router.match(Method::Get, "/", params);
                bench::do_not_optimize(h);
            }
        );
        res.print();
    }

    {
        auto res = bench::run(
            "Router: GET /metrics",
            kWarmup, kIter,
            [&]() {
                params.clear();
                auto h = router.match(Method::Get, "/metrics", params);
                bench::do_not_optimize(h);
            }
        );
        res.print();
    }
}

static void bench_param_lookup() {
    bench::section("Router — 파라미터 경로 룩업");

    Router router;
    router.add_route(Method::Get,    "/api/v1/users/:id",             noop_handler);
    router.add_route(Method::Put,    "/api/v1/users/:id",             noop_handler);
    router.add_route(Method::Delete, "/api/v1/users/:id",             noop_handler);
    router.add_route(Method::Get,    "/api/v1/users/:id/orders",      noop_handler);
    router.add_route(Method::Get,    "/api/v1/users/:id/orders/:oid", noop_handler);
    router.add_route(Method::Get,    "/api/v1/products/:sku/reviews", noop_handler);

    constexpr uint64_t kWarmup = 30'000;
    constexpr uint64_t kIter   = 1'000'000;

    std::unordered_map<std::string, std::string> params;

    {
        auto res = bench::run(
            "Router: GET /users/:id (단일 파라미터)",
            kWarmup, kIter,
            [&]() {
                params.clear();
                auto h = router.match(Method::Get, "/api/v1/users/user-123456789", params);
                bench::do_not_optimize(h);
            }
        );
        res.print();
        if (res.avg_ns() < 300.0) {
            bench::pass("파라미터 룩업 목표 달성: < 300 ns");
        } else {
            bench::fail("파라미터 룩업 목표 미달: >= 300 ns");
        }
    }

    {
        auto res = bench::run(
            "Router: GET /users/:id/orders/:oid (이중 파라미터)",
            kWarmup, kIter,
            [&]() {
                params.clear();
                auto h = router.match(Method::Get,
                    "/api/v1/users/usr-999/orders/ord-888", params);
                bench::do_not_optimize(h);
            }
        );
        res.print();
    }
}

static void bench_large_routing_table() {
    bench::section("Router — 대규모 라우팅 테이블 (1000+ 경로)");

    Router router;
    std::vector<std::string> path_store;
    path_store.reserve(1100);

    for (int i = 0; i < 500; ++i) {
        path_store.push_back("/api/v" + std::to_string(i) + "/resource");
        router.add_route(Method::Get, path_store.back(), noop_handler);
    }
    for (int i = 0; i < 500; ++i) {
        path_store.push_back("/svc/" + std::to_string(i) + "/endpoint");
        router.add_route(Method::Post, path_store.back(), noop_handler);
    }
    for (int i = 0; i < 100; ++i) {
        path_store.push_back("/v" + std::to_string(i) + "/items/:id");
        router.add_route(Method::Get, path_store.back(), noop_handler);
    }

    constexpr uint64_t kWarmup = 10'000;
    constexpr uint64_t kIter   = 500'000;

    std::unordered_map<std::string, std::string> params;

    {
        auto res = bench::run(
            "Router: 1100-route — /api/v250/resource (히트)",
            kWarmup, kIter,
            [&]() {
                params.clear();
                auto h = router.match(Method::Get, "/api/v250/resource", params);
                bench::do_not_optimize(h);
            }
        );
        res.print();
        if (res.avg_ns() < 500.0) {
            bench::pass("대규모 테이블 목표 달성: < 500 ns");
        } else {
            bench::fail("대규모 테이블 목표 미달: >= 500 ns");
        }
    }

    {
        auto res = bench::run(
            "Router: 1100-route — /no/such/path (미스/404)",
            kWarmup, kIter,
            [&]() {
                params.clear();
                auto h = router.match(Method::Get, "/no/such/path", params);
                bench::do_not_optimize(h);
            }
        );
        res.print();
    }
}

// ─── 메인 ────────────────────────────────────────────────────────────────────

int main() {
    printf("\n");
    printf("══════════════════════════════════════════════════════════════\n");
    printf("  qbuem-stack v1.0.0 — Router 성능 벤치마크\n");
    printf("══════════════════════════════════════════════════════════════\n");

    bench_static_lookup();
    bench_param_lookup();
    bench_large_routing_table();

    printf("\n══════════════════════════════════════════════════════════════\n");
    printf("  완료\n");
    printf("══════════════════════════════════════════════════════════════\n\n");

    return 0;
}
