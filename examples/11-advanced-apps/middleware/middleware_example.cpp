/**
 * @file examples/middleware_example.cpp
 * @brief Middleware 체인 예시 — CORS, RateLimit, RequestID, SSE, TokenAuth
 */
#include <qbuem_json/qbuem_json.hpp>
#include <qbuem/qbuem_stack.hpp>
#include <qbuem/middleware/cors.hpp>
#include <qbuem/middleware/rate_limit.hpp>
#include <qbuem/middleware/request_id.hpp>
#include <qbuem/middleware/sse.hpp>
#include <qbuem/middleware/token_auth.hpp>
#include <qbuem/middleware/security.hpp>

#include <iostream>
#include <string>

struct PublicResponse    { std::string message; };
QBUEM_JSON_FIELDS(PublicResponse, message)

struct ProtectedResponse { std::string user; };
QBUEM_JSON_FIELDS(ProtectedResponse, user)

using namespace qbuem;
using namespace qbuem::middleware;

// ─── 토큰 검증기 구현 ───────────────────────────────────────────────────────

/// API 키 기반 단순 ITokenVerifier 구현 예시
class ApiKeyVerifier : public ITokenVerifier {
public:
    std::optional<TokenClaims> verify(std::string_view token) noexcept override {
        if (token == "secret-api-key") {
            TokenClaims c;
            c.subject  = "user:42";
            c.issuer   = "example.com";
            c.audience = "api";
            c.exp      = 9999999999L;
            return c;
        }
        return std::nullopt;
    }
};

// ─── 앱 설정 ────────────────────────────────────────────────────────────────

void setup_app(App& app) {
    auto verifier = std::make_shared<ApiKeyVerifier>();

    // 1. CORS 미들웨어 — 특정 오리진 허용
    app.use(cors(CorsConfig{
        .allow_origin      = "https://example.com",
        .allow_methods     = "GET, POST, PUT, DELETE",
        .allow_headers     = "Content-Type, Authorization",
        .allow_credentials = true,
        .max_age           = 3600,
    }));

    // 2. Rate Limit 미들웨어 — 토큰 버킷 (100 req/s, burst=20)
    app.use(rate_limit(RateLimitConfig{
        .rate_per_sec = 100.0,
        .max_keys     = 10'000,
        .burst        = 20.0,
    }));

    // 3. Request ID 미들웨어 — 모든 응답에 X-Request-ID 부여
    app.use(request_id("X-Request-ID"));

    // 4. HSTS (Strict-Transport-Security) 미들웨어
    app.use(hsts(31'536'000, true));

    // 5. API 엔드포인트 — Token Auth 가 적용된 보호된 라우트
    app.use(bearer_auth(verifier));

    // GET /public — 인증 없이 접근 가능
    app.get("/public", [](const Request&, Response& res) {
        res.status(200)
           .header("Content-Type", "application/json")
           .body(qbuem::write(PublicResponse{"public"}));
    });

    // GET /sse — Server-Sent Events 스트림
    app.get("/sse", [](const Request&, Response& res) {
        SseStream sse(res);
        sse.send("connected",    "status",  "1");
        sse.send("heartbeat",    "ping",    "2", 30000); // retry=30s
        sse.send("data payload", "message", "3");
        sse.heartbeat();  // ": ping\n\n"
        sse.close();
    });

    // GET /protected — 인증 필요
    app.get("/protected", [](const Request& req, Response& res) {
        auto sub = req.header("X-Auth-Sub");
        res.status(200)
           .header("Content-Type", "application/json")
           .body(qbuem::write(ProtectedResponse{std::string(sub)}));
    });
}

int main() {
    App app(2);
    setup_app(app);

    std::cout << "[middleware] App configured:\n"
              << "  - CORS (example.com, credentials)\n"
              << "  - RateLimit (100 req/s, burst=20)\n"
              << "  - RequestID (X-Request-ID)\n"
              << "  - HSTS (max-age=31536000)\n"
              << "  - BearerAuth (ApiKeyVerifier)\n"
              << "  - GET /public, /sse, /protected\n";
    return 0;
}
