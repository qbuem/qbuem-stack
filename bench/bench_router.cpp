/**
 * @file bench/bench_router.cpp
 * @brief Radix Tree Router performance benchmark.
 *
 * ### Measured Items
 * - Static route lookup latency (ns/lookup)
 * - Parameter route lookup latency (e.g., /user/:id)
 * - Worst-case lookup after 1000 routes registered
 * - 404 miss case latency
 *
 * ### Performance Goals (v1.0)
 * - Static lookup  : < 200 ns
 * - Param lookup   : < 300 ns
 * - 1000-route     : < 500 ns (worst case)
 */

#include "bench_common.hpp"

#include <qbuem/http/request.hpp>
#include <qbuem/http/router.hpp>

#include <print>
#include <string>
#include <unordered_map>
#include <vector>

using qbuem::Method;
using qbuem::Router;

// ─── Dummy handler ────────────────────────────────────────────────────────────

static qbuem::Handler noop_handler = [](const qbuem::Request&, qbuem::Response&) {};

// ─── Benchmarks ───────────────────────────────────────────────────────────────

static void bench_static_lookup() {
    bench::section("Router — Static Route Lookup");

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
            "Router: GET /api/v1/health (static)",
            kWarmup, kIter,
            [&]() {
                params.clear();
                auto h = router.match(Method::Get, "/api/v1/health", params);
                bench::do_not_optimize(h);
            }
        );
        res.print();
        if (res.avg_ns() < 200.0) {
            bench::pass("Static lookup goal met: < 200 ns");
        } else {
            bench::fail("Static lookup goal missed: >= 200 ns");
        }
    }

    {
        auto res = bench::run(
            "Router: GET / (root)",
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
    bench::section("Router — Parameter Route Lookup");

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
            "Router: GET /users/:id (single param)",
            kWarmup, kIter,
            [&]() {
                params.clear();
                auto h = router.match(Method::Get, "/api/v1/users/user-123456789", params);
                bench::do_not_optimize(h);
            }
        );
        res.print();
        if (res.avg_ns() < 300.0) {
            bench::pass("Param lookup goal met: < 300 ns");
        } else {
            bench::fail("Param lookup goal missed: >= 300 ns");
        }
    }

    {
        auto res = bench::run(
            "Router: GET /users/:id/orders/:oid (double param)",
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
    bench::section("Router — Large Routing Table (1000+ routes)");

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
            "Router: 1100-route — /api/v250/resource (hit)",
            kWarmup, kIter,
            [&]() {
                params.clear();
                auto h = router.match(Method::Get, "/api/v250/resource", params);
                bench::do_not_optimize(h);
            }
        );
        res.print();
        if (res.avg_ns() < 500.0) {
            bench::pass("Large table goal met: < 500 ns");
        } else {
            bench::fail("Large table goal missed: >= 500 ns");
        }
    }

    {
        auto res = bench::run(
            "Router: 1100-route — /no/such/path (miss/404)",
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

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    std::println();
    std::println("══════════════════════════════════════════════════════════════");
    std::println("  qbuem-stack — Router Performance Benchmark");
    std::println("══════════════════════════════════════════════════════════════");

    bench_static_lookup();
    bench_param_lookup();
    bench_large_routing_table();

    std::println();
    std::println("══════════════════════════════════════════════════════════════");
    std::println("  Done");
    std::println("══════════════════════════════════════════════════════════════");
    std::println();

    return 0;
}
