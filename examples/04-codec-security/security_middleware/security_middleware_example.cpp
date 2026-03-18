/**
 * @file security_middleware_example.cpp
 * @brief Security middleware + authentication + static file serving example.
 *
 * ## Coverage — middleware/security.hpp
 * - hsts()                     — Strict-Transport-Security header
 * - csp()                      — Content-Security-Policy header
 * - x_frame_options()          — X-Frame-Options header
 * - x_content_type_options()   — X-Content-Type-Options header
 * - referrer_policy()          — Referrer-Policy header
 * - permissions_policy()       — Permissions-Policy header
 * - secure_headers()           — apply all security headers at once
 *
 * ## Coverage — middleware/token_auth.hpp (jwt.hpp shim included)
 * - ITokenVerifier             — token verification interface
 * - bearer_auth()              — Bearer token middleware
 *
 * ## Coverage — middleware/static_files.hpp
 * - mime_type()                — extension → MIME type mapping
 * - serve_file()               — build file response
 *
 * ## Coverage — middleware/body_encoder.hpp (compress.hpp shim included)
 * - IBodyEncoder               — body encoder interface
 * - compress()                 — apply encoder middleware
 * - compress_response()        — encode response body inside a handler
 */

#include <qbuem/http/request.hpp>
#include <qbuem/http/response.hpp>
#include <qbuem/http/router.hpp>
#include <qbuem/middleware/body_encoder.hpp>
#include <qbuem/middleware/compress.hpp>
#include <qbuem/middleware/jwt.hpp>
#include <qbuem/middleware/security.hpp>
#include <qbuem/middleware/static_files.hpp>
#include <qbuem/middleware/token_auth.hpp>

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include <qbuem/compat/print.hpp>

using namespace qbuem;
using namespace qbuem::middleware;
using std::println;
using std::print;

// ─────────────────────────────────────────────────────────────────────────────
// §1  Security headers
// ─────────────────────────────────────────────────────────────────────────────

static void demo_security_headers() {
    println("── §1  Security Header Middleware ──");

    Request  req;
    Response res;

    // Apply individual security headers
    hsts(31536000, true, false)(req, res);
    csp("default-src 'self'; img-src *")(req, res);
    x_frame_options("DENY")(req, res);
    x_content_type_options()(req, res);
    referrer_policy("strict-origin-when-cross-origin")(req, res);
    permissions_policy("camera=(), microphone=()")(req, res);

    println("  HSTS:     {}",
                std::string(res.get_header("Strict-Transport-Security")));
    println("  CSP:      {}",
                std::string(res.get_header("Content-Security-Policy")));
    println("  X-Frame:  {}",
                std::string(res.get_header("X-Frame-Options")));
    println("  X-CTO:    {}",
                std::string(res.get_header("X-Content-Type-Options")));
    println("  Referrer: {}",
                std::string(res.get_header("Referrer-Policy")));
    println("  Perms:    {}",
                std::string(res.get_header("Permissions-Policy")));

    // Combined secure_headers() — apply all security headers at once
    Response res2;
    secure_headers()(req, res2);
    bool has_hsts = !res2.get_header("Strict-Transport-Security").empty();
    println("  secure_headers(): includes HSTS = {}\n",
                has_hsts ? "yes" : "no");
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  Bearer token authentication (token_auth.hpp / jwt.hpp shim)
// ─────────────────────────────────────────────────────────────────────────────

static void demo_bearer_auth() {
    println("── §2  Bearer Token Authentication ──");

    // Custom token verifier implementation.
    // ITokenVerifier::verify() must be noexcept.
    // TokenClaims fields: subject, issuer, audience, exp, nbf, custom.
    struct MockVerifier : ITokenVerifier {
        std::optional<TokenClaims> verify(std::string_view token) noexcept override {
            if (token == "valid-token-abc123") {
                TokenClaims claims;
                claims.subject = "user-42";
                claims.custom["roles"] = "read,write";
                return claims;
            }
            return std::nullopt;
        }
    };

    auto verifier = std::make_shared<MockVerifier>();
    auto auth_mw  = bearer_auth(verifier);

    // Valid token
    // On success, bearer_auth() forwards claims into res headers:
    //   X-Auth-Sub  → claims.subject
    {
        Request req;
        req.add_header("Authorization", "Bearer valid-token-abc123");
        Response res;
        bool ok = auth_mw(req, res);
        println("  Valid token: {} (sub={})",
                    ok ? "pass" : "blocked",
                    ok ? std::string(res.get_header("X-Auth-Sub")) : "-");
    }

    // Invalid token
    {
        Request req;
        req.add_header("Authorization", "Bearer wrong-token");
        Response res;
        bool ok = auth_mw(req, res);
        println("  Invalid token: {} (status={})",
                    ok ? "pass" : "blocked", res.status_code());
    }

    // No header
    {
        Request req;
        Response res;
        bool ok = auth_mw(req, res);
        println("  No header: {}\n", ok ? "pass" : "blocked");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// §3  API Key authentication
// ApiKeyAuth is not provided by token_auth.hpp; implement inline here.
// ─────────────────────────────────────────────────────────────────────────────

class ApiKeyAuth {
public:
    explicit ApiKeyAuth(std::unordered_set<std::string> keys)
        : keys_(std::move(keys)) {}

    Middleware middleware(std::string_view header_name) const {
        return [keys = keys_, hdr = std::string(header_name)](
                   const Request &req, Response &res) -> bool {
            std::string_view key = req.header(hdr);
            if (key.empty() || keys.find(std::string(key)) == keys.end()) {
                res.status(401).body("Unauthorized");
                return false;
            }
            return true;
        };
    }

private:
    std::unordered_set<std::string> keys_;
};

static void demo_api_key() {
    println("── §3  API Key Authentication ──");

    // Set of allowed API keys
    ApiKeyAuth api_auth({"key-prod-001", "key-prod-002", "key-dev-999"});
    auto mw = api_auth.middleware("X-API-Key");

    struct TestCase { const char* key; bool expect_pass; };
    std::vector<TestCase> cases = {
        {"key-prod-001", true},
        {"key-dev-999",  true},
        {"invalid-key",  false},
        {"",             false},
    };

    for (auto& tc : cases) {
        Request req;
        Response res;
        if (tc.key[0]) req.add_header("X-API-Key", tc.key);
        bool ok = mw(req, res);
        println("  key='{}': {} {}",
                    tc.key, ok ? "pass" : "blocked",
                    (ok == tc.expect_pass) ? "OK" : "FAIL");
    }
    println("");
}

// ─────────────────────────────────────────────────────────────────────────────
// §4  Static file serving utilities
// ─────────────────────────────────────────────────────────────────────────────

static void demo_static_files() {
    println("── §4  Static File Utilities ──");

    // MIME type detection
    struct MimeCase { const char* ext; };
    std::vector<MimeCase> mimes = {
        {".html"}, {".css"}, {".js"}, {".json"},
        {".png"},  {".svg"}, {".wasm"}, {".unknown"},
    };

    for (auto& m : mimes) {
        println("  mime_type(\"{}\") = {}",
                    m.ext, std::string(mime_type(m.ext)));
    }

    // serve_file(): build response based on whether the file exists
    // Signature: serve_file(string_view fs_path, Response& res) -> void
    Response res;
    // Non-existent path → expect 404 response
    serve_file("/tmp/nonexistent_file_qbuem.html", res);
    println("  serve_file(missing file): status={}\n", res.status_code());
}

// ─────────────────────────────────────────────────────────────────────────────
// §5  Body Encoder (compress.hpp shim → body_encoder.hpp)
// ─────────────────────────────────────────────────────────────────────────────

// IBodyEncoder concrete implementation: identity (passthrough).
// encode(string_view src, string& dst) -> bool
// encoding_name() -> string_view
// accept_token()  -> string_view
class IdentityEncoder : public IBodyEncoder {
public:
    bool encode(std::string_view src, std::string &dst) noexcept override {
        dst.assign(src.data(), src.size());
        return true;
    }
    std::string_view encoding_name() const noexcept override { return "identity"; }
    std::string_view accept_token()  const noexcept override { return "identity"; }
};

static void demo_body_encoder() {
    println("── §5  Body Encoder ──");

    // IdentityEncoder — passthrough without transformation
    IdentityEncoder identity;
    std::string body = "Hello, qbuem body encoder!";
    std::string encoded;
    bool ok = identity.encode(body, encoded);
    println("  IdentityEncoder: in={} out={} same={}",
                body.size(), encoded.size(),
                (ok && body == encoded) ? "yes" : "no");
    println("  content-encoding: {}", identity.encoding_name());

    // compress() middleware: returned Middleware always returns true (pre-handler stub).
    // compress_response() is the in-handler post-processing helper.
    auto mw = compress(identity);

    Request req;
    Response res;
    res.body(body);
    mw(req, res);
    println("  body size after compress() middleware: {}\n",
                res.get_body().size());
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    println("=== qbuem Security Middleware Example ===\n");

    demo_security_headers();
    demo_bearer_auth();
    demo_api_key();
    demo_static_files();
    demo_body_encoder();

    println("=== Done ===");
    return 0;
}
