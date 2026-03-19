/**
 * @file examples/middleware_example.cpp
 * @brief Middleware chain example — CORS, RateLimit, RequestID, SSE, TokenAuth
 */
#include <qbuem_json/qbuem_json.hpp>
#include <qbuem/qbuem_stack.hpp>
#include <qbuem/middleware/cors.hpp>
#include <qbuem/middleware/rate_limit.hpp>
#include <qbuem/middleware/request_id.hpp>
#include <qbuem/middleware/sse.hpp>
#include <qbuem/middleware/token_auth.hpp>
#include <qbuem/middleware/security.hpp>

#include <print>
#include <string>

struct PublicResponse    { std::string message; };
QBUEM_JSON_FIELDS(PublicResponse, message)

struct ProtectedResponse { std::string user; };
QBUEM_JSON_FIELDS(ProtectedResponse, user)

using namespace qbuem;
using namespace qbuem::middleware;

// ─── Token verifier implementation ──────────────────────────────────────────

/// Simple API-key-based ITokenVerifier implementation example.
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

// ─── App setup ──────────────────────────────────────────────────────────────

void setup_app(App& app) {
    auto verifier = std::make_shared<ApiKeyVerifier>();

    // 1. CORS middleware — allow specific origin
    app.use(cors(CorsConfig{
        .allow_origin      = "https://example.com",
        .allow_methods     = "GET, POST, PUT, DELETE",
        .allow_headers     = "Content-Type, Authorization",
        .allow_credentials = true,
        .max_age           = 3600,
    }));

    // 2. Rate Limit middleware — token bucket (100 req/s, burst=20)
    app.use(rate_limit(RateLimitConfig{
        .rate_per_sec = 100.0,
        .max_keys     = 10'000,
        .burst        = 20.0,
    }));

    // 3. Request ID middleware — attach X-Request-ID to every response
    app.use(request_id("X-Request-ID"));

    // 4. HSTS (Strict-Transport-Security) middleware
    app.use(hsts(31'536'000, true));

    // 5. API endpoints — protected routes requiring Token Auth
    app.use(bearer_auth(verifier));

    // GET /public — accessible without authentication
    app.get("/public", [](const Request&, Response& res) {
        res.status(200)
           .header("Content-Type", "application/json")
           .body(qbuem::write(PublicResponse{"public"}));
    });

    // GET /sse — Server-Sent Events stream
    app.get("/sse", [](const Request&, Response& res) {
        SseStream sse(res);
        sse.send("connected",    "status",  "1");
        sse.send("heartbeat",    "ping",    "2", 30000); // retry=30s
        sse.send("data payload", "message", "3");
        sse.heartbeat();  // ": ping\n\n"
        sse.close();
    });

    // GET /protected — authentication required
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

    std::println("[middleware] App configured:");
    std::println("  - CORS (example.com, credentials)");
    std::println("  - RateLimit (100 req/s, burst=20)");
    std::println("  - RequestID (X-Request-ID)");
    std::println("  - HSTS (max-age=31536000)");
    std::println("  - BearerAuth (ApiKeyVerifier)");
    std::println("  - GET /public, /sse, /protected");
    return 0;
}
