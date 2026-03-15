/**
 * @file examples/middleware_example.cpp
 * @brief Middleware 체인 예시 — CORS, RateLimit, RequestID, SSE, TokenAuth
 */
#include <qbuem/http/router.hpp>
#include <qbuem/http/request.hpp>
#include <qbuem/http/response.hpp>
#include <qbuem/middleware/cors.hpp>
#include <qbuem/middleware/rate_limit.hpp>
#include <qbuem/middleware/request_id.hpp>
#include <qbuem/middleware/sse.hpp>
#include <qbuem/middleware/token_auth.hpp>
#include <qbuem/middleware/security.hpp>

#include <iostream>
#include <string>

using namespace qbuem;
using namespace qbuem::middleware;

// ─── 토큰 검증기 구현 ───────────────────────────────────────────────────────

/// API 키 기반 단순 ITokenVerifier 구현 예시
class ApiKeyVerifier : public ITokenVerifier {
public:
    std::optional<TokenClaims> verify(std::string_view token) noexcept override {
        if (token == "secret-api-key") {
            return TokenClaims{
                .subject  = "user:42",
                .issuer   = "example.com",
                .audience = "api",
                .exp      = 9999999999L,
            };
        }
        return std::nullopt;
    }
};

// ─── 라우터 설정 ────────────────────────────────────────────────────────────

void setup_router(Http::Router& router) {

    // 1. CORS 미들웨어 — 특정 오리진 허용
    router.use(cors(CorsConfig{
        .allow_origin      = "https://example.com",
        .allow_methods     = "GET, POST, PUT, DELETE",
        .allow_headers     = "Content-Type, Authorization",
        .allow_credentials = true,
        .max_age           = 3600,
    }));

    // 2. Rate Limit 미들웨어 — 토큰 버킷 (100 req/s, burst=20)
    router.use(rate_limit(RateLimitConfig{
        .rate_per_sec = 100.0,
        .burst        = 20.0,
        .max_keys     = 10'000,
    }));

    // 3. Request ID 미들웨어 — 모든 응답에 X-Request-ID 부여
    router.use(request_id("X-Request-ID"));

    // 4. HSTS (Strict-Transport-Security) 미들웨어
    router.use(hsts(HstsConfig{
        .max_age             = 31536000,
        .include_subdomains  = true,
        .preload             = false,
    }));

    // 5. API 엔드포인트 — Token Auth 가 적용된 보호된 라우트
    ApiKeyVerifier verifier;
    router.use(bearer_auth(verifier));

    // GET /public — 인증 없이 접근 가능 (rate_limit + request_id 만 적용)
    router.get("/public", [](const Request& req, Response& res) {
        auto rid = res.header("X-Request-ID");
        res.status(200).json(R"({"message":"public","request_id":")" + rid + R"("})");
    });

    // GET /sse — Server-Sent Events 스트림
    router.get("/sse", [](const Request& /*req*/, Response& res) {
        SseStream sse(res);
        sse.send("connected",  "status",  "1");
        sse.send("heartbeat",  "ping",    "2", 30000); // retry=30s
        sse.send("data payload", "message", "3");
        sse.heartbeat();  // ": ping\n\n"
        sse.close();
    });

    // GET /protected — 인증 필요
    router.get("/protected", [](const Request& req, Response& res) {
        auto sub = req.header("X-Auth-Sub");
        res.status(200).json(R"({"user":")" + sub + R"("})");
    });
}

int main() {
    Http::Router router;
    setup_router(router);

    std::cout << "[middleware] Router configured:\n"
              << "  - CORS (example.com, credentials)\n"
              << "  - RateLimit (100 req/s, burst=20)\n"
              << "  - RequestID (X-Request-ID)\n"
              << "  - HSTS (max-age=31536000)\n"
              << "  - BearerAuth (ApiKeyVerifier)\n"
              << "  - GET /public, /sse, /protected\n";
    return 0;
}
