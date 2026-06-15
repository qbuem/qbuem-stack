/**
 * @file tests/router_test.cpp
 * @brief Unit tests for qbuem::Router / RadixTree route matching.
 *
 * Coverage:
 *   - Static route hit (exact path)
 *   - Param route (/users/:id) — extracts the :id segment
 *   - Multi-param route (/users/:uid/posts/:pid)
 *   - Wildcard / prefix route (add_prefix_route → "**" suffix capture)
 *   - Trailing slash distinction (/users vs /users/)
 *   - Method mismatch (GET-registered path queried as POST → no match)
 *   - Miss / 404 (unregistered path → std::monostate)
 *   - path_exists() (distinguishes 404 path-unknown from 405 method-not-allowed)
 *
 * Handler identity is verified behaviorally: each registered handler writes a
 * unique status code and body to the Response, so invoking the matched handler
 * and inspecting the Response proves WHICH handler was selected.
 *
 * Linking note: the mandated standalone self-verify command compiles ONLY this
 * translation unit plus the gtest archives — it does NOT link libqbuem_http.
 * Router / RadixTree / Response member functions are out-of-line (in src/http/).
 * To keep the standalone command self-contained we #include those .cpp sources
 * directly into this TU. When built through the project's CMake (which links
 * libqbuem_http), define QBUEM_ROUTER_TEST_LINK_LIB to skip the inline sources
 * and avoid duplicate symbols.
 */

#include <gtest/gtest.h>
#include <qbuem/http/router.hpp>

#ifndef QBUEM_ROUTER_TEST_LINK_LIB
#include "../src/http/response.cpp"
#include "../src/http/router.cpp"
#endif

#include <string>
#include <unordered_map>
#include <variant>

using namespace qbuem;

namespace {

// Build a sync Handler that stamps a unique marker into the Response so that,
// after match() returns a HandlerVariant, invoking it proves which handler ran.
Handler marker_handler(int code, std::string body) {
    return [code, body = std::move(body)](const Request&, Response& res) {
        res.status(code).body(body);
    };
}

// Match against a router and, if a sync Handler matched, invoke it and return
// the body it wrote. Returns std::nullopt when no Handler matched (404 / 405 /
// async handler). `out_params` receives the extracted path parameters.
struct MatchResult {
    bool is_handler = false;       // matched a sync Handler
    bool is_monostate = true;      // no match at all
    int  status = 0;
    std::string body;
};

MatchResult run_match(const Router& r, Method m, std::string_view path,
                      std::unordered_map<std::string, std::string>& params) {
    MatchResult out;
    HandlerVariant hv = r.match(m, path, params);
    out.is_monostate = std::holds_alternative<std::monostate>(hv);
    if (std::holds_alternative<Handler>(hv)) {
        out.is_handler = true;
        Request req;
        Response res;
        std::get<Handler>(hv)(req, res);
        out.status = res.status_code();
        out.body = std::string(res.get_body());
    }
    return out;
}

} // namespace

// ─── Static route hit ─────────────────────────────────────────────────────────

TEST(RouterTest, StaticRouteHitInvokesCorrectHandler) {
    Router r;
    r.add_route(Method::Get, "/health", marker_handler(200, "ok-health"));

    std::unordered_map<std::string, std::string> params;
    auto res = run_match(r, Method::Get, "/health", params);

    EXPECT_TRUE(res.is_handler);
    EXPECT_FALSE(res.is_monostate);
    EXPECT_EQ(res.status, 200);
    EXPECT_EQ(res.body, "ok-health");
    EXPECT_TRUE(params.empty());
}

TEST(RouterTest, TwoStaticRoutesResolveToDistinctHandlers) {
    Router r;
    r.add_route(Method::Get, "/a", marker_handler(201, "handler-A"));
    r.add_route(Method::Get, "/b", marker_handler(202, "handler-B"));

    std::unordered_map<std::string, std::string> pa, pb;
    auto ra = run_match(r, Method::Get, "/a", pa);
    auto rb = run_match(r, Method::Get, "/b", pb);

    EXPECT_EQ(ra.body, "handler-A");
    EXPECT_EQ(ra.status, 201);
    EXPECT_EQ(rb.body, "handler-B");
    EXPECT_EQ(rb.status, 202);
}

// ─── Param route — single ──────────────────────────────────────────────────────

TEST(RouterTest, ParamRouteExtractsId) {
    Router r;
    r.add_route(Method::Get, "/users/:id", marker_handler(200, "user-handler"));

    std::unordered_map<std::string, std::string> params;
    auto res = run_match(r, Method::Get, "/users/42", params);

    EXPECT_TRUE(res.is_handler);
    EXPECT_EQ(res.body, "user-handler");
    ASSERT_TRUE(params.contains("id"));
    EXPECT_EQ(params.at("id"), "42");
    EXPECT_EQ(params.size(), 1u);
}

TEST(RouterTest, ParamRouteExtractsNonNumericId) {
    Router r;
    r.add_route(Method::Get, "/users/:id", marker_handler(200, "user-handler"));

    std::unordered_map<std::string, std::string> params;
    auto res = run_match(r, Method::Get, "/users/alice", params);

    EXPECT_TRUE(res.is_handler);
    ASSERT_TRUE(params.contains("id"));
    EXPECT_EQ(params.at("id"), "alice");
}

// ─── Static-vs-param precedence ────────────────────────────────────────────────

TEST(RouterTest, StaticSegmentWinsOverParamSegment) {
    // search_recursive tries the exact-character child before the param child,
    // so "/users/me" must hit the static handler, not the :id param handler.
    Router r;
    r.add_route(Method::Get, "/users/:id", marker_handler(200, "param-user"));
    r.add_route(Method::Get, "/users/me",  marker_handler(200, "static-me"));

    std::unordered_map<std::string, std::string> p_static, p_param;
    auto res_static = run_match(r, Method::Get, "/users/me", p_static);
    auto res_param  = run_match(r, Method::Get, "/users/77", p_param);

    EXPECT_EQ(res_static.body, "static-me");
    EXPECT_FALSE(p_static.contains("id")); // static path captured no param

    EXPECT_EQ(res_param.body, "param-user");
    ASSERT_TRUE(p_param.contains("id"));
    EXPECT_EQ(p_param.at("id"), "77");
}

// ─── Multi-param route ─────────────────────────────────────────────────────────

TEST(RouterTest, MultiParamRouteExtractsBothSegments) {
    Router r;
    r.add_route(Method::Get, "/users/:uid/posts/:pid",
                marker_handler(200, "post-handler"));

    std::unordered_map<std::string, std::string> params;
    auto res = run_match(r, Method::Get, "/users/7/posts/99", params);

    EXPECT_TRUE(res.is_handler);
    EXPECT_EQ(res.body, "post-handler");
    ASSERT_TRUE(params.contains("uid"));
    ASSERT_TRUE(params.contains("pid"));
    EXPECT_EQ(params.at("uid"), "7");
    EXPECT_EQ(params.at("pid"), "99");
    EXPECT_EQ(params.size(), 2u);
}

// ─── Wildcard / prefix route ───────────────────────────────────────────────────

TEST(RouterTest, PrefixRouteCapturesSuffixAsDoubleStar) {
    Router r;
    HandlerVariant hv = Handler{marker_handler(200, "static-files")};
    r.add_prefix_route(Method::Get, "/static/", std::move(hv));

    std::unordered_map<std::string, std::string> params;
    auto res = run_match(r, Method::Get, "/static/css/app.css", params);

    EXPECT_TRUE(res.is_handler);
    EXPECT_EQ(res.body, "static-files");
    ASSERT_TRUE(params.contains("**"));
    EXPECT_EQ(params.at("**"), "css/app.css");
}

TEST(RouterTest, PrefixRouteEmptySuffixWhenPathEqualsPrefix) {
    Router r;
    r.add_prefix_route(Method::Get, "/files/",
                       HandlerVariant{Handler{marker_handler(200, "files")}});

    std::unordered_map<std::string, std::string> params;
    auto res = run_match(r, Method::Get, "/files/", params);

    EXPECT_TRUE(res.is_handler);
    EXPECT_EQ(res.body, "files");
    ASSERT_TRUE(params.contains("**"));
    EXPECT_EQ(params.at("**"), "");
}

TEST(RouterTest, ExactRouteTakesPrecedenceOverPrefixRoute) {
    // match() consults the RadixTree (exact/param) before prefix routes.
    Router r;
    r.add_prefix_route(Method::Get, "/static/",
                       HandlerVariant{Handler{marker_handler(200, "prefix")}});
    r.add_route(Method::Get, "/static/special",
                marker_handler(200, "exact-special"));

    std::unordered_map<std::string, std::string> p_exact, p_prefix;
    auto res_exact  = run_match(r, Method::Get, "/static/special", p_exact);
    auto res_prefix = run_match(r, Method::Get, "/static/other", p_prefix);

    EXPECT_EQ(res_exact.body, "exact-special");
    EXPECT_FALSE(p_exact.contains("**")); // exact match did not go through prefix

    EXPECT_EQ(res_prefix.body, "prefix");
    ASSERT_TRUE(p_prefix.contains("**"));
    EXPECT_EQ(p_prefix.at("**"), "other");
}

// ─── Trailing slash distinction ────────────────────────────────────────────────

TEST(RouterTest, TrailingSlashIsDistinctFromNoSlash) {
    Router r;
    r.add_route(Method::Get, "/users",  marker_handler(200, "no-slash"));
    r.add_route(Method::Get, "/users/", marker_handler(200, "with-slash"));

    std::unordered_map<std::string, std::string> p1, p2;
    auto r1 = run_match(r, Method::Get, "/users",  p1);
    auto r2 = run_match(r, Method::Get, "/users/", p2);

    EXPECT_EQ(r1.body, "no-slash");
    EXPECT_EQ(r2.body, "with-slash");
}

TEST(RouterTest, TrailingSlashMissWhenOnlyBarePathRegistered) {
    // Only "/users" is registered; "/users/" must NOT match it (radix tree is
    // exact on the trailing character).
    Router r;
    r.add_route(Method::Get, "/users", marker_handler(200, "bare"));

    std::unordered_map<std::string, std::string> params;
    auto res = run_match(r, Method::Get, "/users/", params);

    EXPECT_TRUE(res.is_monostate);
    EXPECT_FALSE(res.is_handler);
}

// ─── Method mismatch (405-style) ───────────────────────────────────────────────

TEST(RouterTest, MethodMismatchYieldsNoHandler) {
    Router r;
    r.add_route(Method::Get, "/orders", marker_handler(200, "get-orders"));

    std::unordered_map<std::string, std::string> params;
    auto res = run_match(r, Method::Post, "/orders", params);

    EXPECT_TRUE(res.is_monostate);
    EXPECT_FALSE(res.is_handler);
    // But the path IS registered (for some method) — distinguishes 405 from 404.
    EXPECT_TRUE(r.path_exists("/orders"));
}

TEST(RouterTest, SamePathDifferentMethodsResolveIndependently) {
    Router r;
    r.add_route(Method::Get,  "/item", marker_handler(200, "GET-item"));
    r.add_route(Method::Post, "/item", marker_handler(201, "POST-item"));

    std::unordered_map<std::string, std::string> pg, pp;
    auto rg = run_match(r, Method::Get,  "/item", pg);
    auto rp = run_match(r, Method::Post, "/item", pp);

    EXPECT_EQ(rg.body, "GET-item");
    EXPECT_EQ(rg.status, 200);
    EXPECT_EQ(rp.body, "POST-item");
    EXPECT_EQ(rp.status, 201);
}

// ─── Miss / 404 ────────────────────────────────────────────────────────────────

TEST(RouterTest, UnregisteredPathIsMiss) {
    Router r;
    r.add_route(Method::Get, "/known", marker_handler(200, "known"));

    std::unordered_map<std::string, std::string> params;
    auto res = run_match(r, Method::Get, "/unknown", params);

    EXPECT_TRUE(res.is_monostate);
    EXPECT_FALSE(res.is_handler);
    EXPECT_FALSE(r.path_exists("/unknown")); // true 404: path unknown
}

TEST(RouterTest, EmptyRouterMatchesNothing) {
    Router r;
    std::unordered_map<std::string, std::string> params;
    auto res = run_match(r, Method::Get, "/anything", params);

    EXPECT_TRUE(res.is_monostate);
    EXPECT_FALSE(r.path_exists("/anything"));
}

TEST(RouterTest, PartialPathPrefixOfParamRouteIsMiss) {
    // "/users/:id" registered; bare "/users" (no trailing segment) must miss —
    // the param child requires a value after "/users/".
    Router r;
    r.add_route(Method::Get, "/users/:id", marker_handler(200, "user"));

    std::unordered_map<std::string, std::string> params;
    auto res = run_match(r, Method::Get, "/users", params);

    EXPECT_TRUE(res.is_monostate);
    EXPECT_FALSE(res.is_handler);
}

// ─── path_exists across methods ────────────────────────────────────────────────

TEST(RouterTest, PathExistsScansAllMethods) {
    Router r;
    r.add_route(Method::Delete, "/resource/:id", marker_handler(204, "del"));

    // Registered only for DELETE; path_exists scans every method, so it is true.
    EXPECT_TRUE(r.path_exists("/resource/abc"));
    EXPECT_FALSE(r.path_exists("/nope"));

    // And a GET on that path is a method mismatch (no handler), while the param
    // is still extracted on the matching DELETE.
    std::unordered_map<std::string, std::string> pget, pdel;
    auto rget = run_match(r, Method::Get,    "/resource/abc", pget);
    auto rdel = run_match(r, Method::Delete, "/resource/abc", pdel);

    EXPECT_TRUE(rget.is_monostate);
    EXPECT_EQ(rdel.body, "del");
    ASSERT_TRUE(pdel.contains("id"));
    EXPECT_EQ(pdel.at("id"), "abc");
}
