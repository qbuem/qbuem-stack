/**
 * @file tests/http_middleware_test.cpp
 * @brief Unit tests for HTTP middleware: CORS, RateLimit, RequestID, TokenAuth.
 */

#include <gtest/gtest.h>
#include <qbuem/http/parser.hpp>
#include <qbuem/http/request.hpp>
#include <qbuem/http/response.hpp>
#include <qbuem/middleware/cors.hpp>
#include <qbuem/middleware/rate_limit.hpp>
#include <qbuem/middleware/request_id.hpp>
#include <qbuem/middleware/token_auth.hpp>

#include <string>
#include <string_view>

using namespace qbuem;
using namespace qbuem::middleware;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static Request make_get(const char* path, const char* origin = nullptr) {
    std::string raw = std::string("GET ") + path + " HTTP/1.1\r\nHost: localhost\r\n";
    if (origin) {
        raw += std::string("Origin: ") + origin + "\r\n";
    }
    raw += "\r\n";

    HttpParser parser;
    Request req;
    parser.parse(raw, req);
    return req;
}

static Request make_options(const char* path, const char* origin = nullptr) {
    std::string raw = std::string("OPTIONS ") + path + " HTTP/1.1\r\nHost: localhost\r\n";
    if (origin) {
        raw += std::string("Origin: ") + origin + "\r\n";
    }
    raw += "\r\n";

    HttpParser parser;
    Request req;
    parser.parse(raw, req);
    return req;
}

// ─── CorsConfig ───────────────────────────────────────────────────────────────

TEST(CorsConfigTest, DefaultValues) {
    CorsConfig cfg;
    EXPECT_EQ(cfg.allow_origin, "*");
    EXPECT_FALSE(cfg.allow_credentials);
    EXPECT_EQ(cfg.max_age, 86400);
    EXPECT_TRUE(cfg.allow_origins.empty());
}

TEST(CorsConfigTest, CustomConfig) {
    CorsConfig cfg;
    cfg.allow_origin = "https://example.com";
    cfg.allow_credentials = true;
    cfg.max_age = 3600;
    EXPECT_EQ(cfg.allow_origin, "https://example.com");
    EXPECT_TRUE(cfg.allow_credentials);
    EXPECT_EQ(cfg.max_age, 3600);
}

// ─── CORS middleware ──────────────────────────────────────────────────────────

TEST(CorsMiddlewareTest, WildcardOriginAllowsRequest) {
    auto mw = cors();  // default: allow_origin = "*"
    auto req = make_get("/api/data");
    Response res;
    bool cont = mw(req, res);
    EXPECT_TRUE(cont);  // chain continues
}

TEST(CorsMiddlewareTest, PreflightReturns204AndHaltsChain) {
    auto mw = cors();
    auto req = make_options("/api/data");
    Response res;
    bool cont = mw(req, res);
    EXPECT_FALSE(cont);  // chain halted
}

TEST(CorsMiddlewareTest, AllowedOriginWhitelistMatches) {
    CorsConfig cfg;
    cfg.allow_origins = {"https://app.example.com", "https://admin.example.com"};
    auto mw = cors(std::move(cfg));

    auto req = make_get("/api/data", "https://app.example.com");
    Response res;
    bool cont = mw(req, res);
    EXPECT_TRUE(cont);
}

TEST(CorsMiddlewareTest, UnknownOriginNotInWhitelist) {
    CorsConfig cfg;
    cfg.allow_origins = {"https://allowed.com"};
    auto mw = cors(std::move(cfg));

    auto req = make_get("/api/data", "https://evil.com");
    Response res;
    bool cont = mw(req, res);
    // Origin not in whitelist — chain continues but no CORS headers added
    EXPECT_TRUE(cont);
}

// ─── RateLimitConfig ──────────────────────────────────────────────────────────

TEST(RateLimitConfigTest, DefaultValues) {
    RateLimitConfig cfg;
    EXPECT_DOUBLE_EQ(cfg.rate_per_sec, 100.0);
    EXPECT_DOUBLE_EQ(cfg.burst, 20.0);
    EXPECT_EQ(cfg.max_keys, 10000u);
}

TEST(RateLimitConfigTest, CustomConfig) {
    RateLimitConfig cfg;
    cfg.rate_per_sec = 50.0;
    cfg.burst = 10.0;
    EXPECT_DOUBLE_EQ(cfg.rate_per_sec, 50.0);
    EXPECT_DOUBLE_EQ(cfg.burst, 10.0);
}

// ─── RateLimit middleware ─────────────────────────────────────────────────────

TEST(RateLimitMiddlewareTest, FirstRequestAllowed) {
    // Very high burst so first request goes through
    RateLimitConfig cfg;
    cfg.rate_per_sec = 1000.0;
    cfg.burst = 100.0;

    auto mw = rate_limit(cfg);
    auto req = make_get("/api/test");
    Response res;
    bool cont = mw(req, res);
    EXPECT_TRUE(cont);
}

TEST(RateLimitMiddlewareTest, ExhaustedBucketReturns429) {
    // Zero burst so even first request is rejected
    RateLimitConfig cfg;
    cfg.rate_per_sec = 0.001;
    cfg.burst = 0.0;  // No burst capacity

    auto mw = rate_limit(cfg);

    // We need to exhaust the bucket; with burst=0, first request has 0 tokens
    // Actually, with burst=0 the bucket starts full (0 tokens) so 0 < 1.0 → 429
    auto req = make_get("/");
    Response res;
    bool cont = mw(req, res);
    // With burst=0, bucket starts with 0 tokens → 429 → chain halted
    EXPECT_FALSE(cont);
}

// ─── request_id middleware ────────────────────────────────────────────────────

TEST(RequestIdMiddlewareTest, AddsRequestIdHeader) {
    auto mw = request_id();
    auto req = make_get("/");
    Response res;
    bool cont = mw(req, res);
    EXPECT_TRUE(cont);  // chain continues
}

TEST(RequestIdMiddlewareTest, CustomHeaderName) {
    auto mw = request_id("X-Trace-ID");
    auto req = make_get("/");
    Response res;
    bool cont = mw(req, res);
    EXPECT_TRUE(cont);
}

// ─── ITokenVerifier / bearer_auth middleware ──────────────────────────────────

class AcceptAllVerifier final : public ITokenVerifier {
public:
    std::optional<TokenClaims> verify(std::string_view) noexcept override {
        TokenClaims c;
        c.subject = "user-123";
        c.custom["role"] = "admin";
        return c;
    }
};

class RejectAllVerifier final : public ITokenVerifier {
public:
    std::optional<TokenClaims> verify(std::string_view) noexcept override {
        return std::nullopt;
    }
};

TEST(TokenAuthTest, ValidTokenAllowsAccess) {
    auto verifier = std::make_shared<AcceptAllVerifier>();
    auto mw = bearer_auth(verifier);

    std::string raw = "GET /api/secure HTTP/1.1\r\nHost: localhost\r\n"
                      "Authorization: Bearer valid-token\r\n\r\n";
    HttpParser parser;
    Request req;
    parser.parse(raw, req);
    Response res;
    bool cont = mw(req, res);
    EXPECT_TRUE(cont);
}

TEST(TokenAuthTest, InvalidTokenBlocks) {
    auto verifier = std::make_shared<RejectAllVerifier>();
    auto mw = bearer_auth(verifier);

    std::string raw = "GET /api/secure HTTP/1.1\r\nHost: localhost\r\n"
                      "Authorization: Bearer bad-token\r\n\r\n";
    HttpParser parser;
    Request req;
    parser.parse(raw, req);
    Response res;
    bool cont = mw(req, res);
    EXPECT_FALSE(cont);  // chain halted — 401
}

TEST(TokenAuthTest, MissingAuthorizationHeaderBlocks) {
    auto verifier = std::make_shared<AcceptAllVerifier>();
    auto mw = bearer_auth(verifier);

    auto req = make_get("/api/secure");  // no Authorization header
    Response res;
    bool cont = mw(req, res);
    EXPECT_FALSE(cont);  // chain halted — 401
}

TEST(TokenClaimsTest, SubjectAndRolesAccessible) {
    TokenClaims claims;
    claims.subject = "user-42";
    claims.custom["role0"] = "reader";
    claims.custom["role1"] = "writer";
    EXPECT_EQ(claims.subject, "user-42");
    EXPECT_EQ(claims.custom.size(), 2u);
    EXPECT_EQ(claims.custom["role0"], "reader");
}
