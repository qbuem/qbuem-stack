/**
 * @file security_middleware_example.cpp
 * @brief 보안 미들웨어 + 인증 + 정적 파일 서빙 예제.
 *
 * ## 커버리지 — middleware/security.hpp
 * - hsts()                     — Strict-Transport-Security 헤더
 * - csp()                      — Content-Security-Policy 헤더
 * - x_frame_options()          — X-Frame-Options 헤더
 * - x_content_type_options()   — X-Content-Type-Options 헤더
 * - referrer_policy()          — Referrer-Policy 헤더
 * - permissions_policy()       — Permissions-Policy 헤더
 * - secure_headers()           — 모든 보안 헤더 일괄 적용
 *
 * ## 커버리지 — middleware/token_auth.hpp (jwt.hpp shim 포함)
 * - ITokenVerifier             — 토큰 검증 인터페이스
 * - bearer_auth()              — Bearer 토큰 미들웨어
 *
 * ## 커버리지 — middleware/static_files.hpp
 * - mime_type()                — 확장자 → MIME 타입 매핑
 * - serve_file()               — 파일 응답 생성
 *
 * ## 커버리지 — middleware/body_encoder.hpp (compress.hpp shim 포함)
 * - IBodyEncoder               — 바디 인코더 인터페이스
 * - compress()                 — 인코더 적용 미들웨어
 * - compress_response()        — 핸들러 내에서 응답 바디 인코딩
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

#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

using namespace qbuem;
using namespace qbuem::middleware;

// ─────────────────────────────────────────────────────────────────────────────
// §1  보안 헤더
// ─────────────────────────────────────────────────────────────────────────────

static void demo_security_headers() {
    std::printf("── §1  보안 헤더 미들웨어 ──\n");

    Request  req;
    Response res;

    // 개별 보안 헤더 적용
    hsts(31536000, true, false)(req, res);
    csp("default-src 'self'; img-src *")(req, res);
    x_frame_options("DENY")(req, res);
    x_content_type_options()(req, res);
    referrer_policy("strict-origin-when-cross-origin")(req, res);
    permissions_policy("camera=(), microphone=()")(req, res);

    std::printf("  HSTS:     %s\n",
                std::string(res.get_header("Strict-Transport-Security")).c_str());
    std::printf("  CSP:      %s\n",
                std::string(res.get_header("Content-Security-Policy")).c_str());
    std::printf("  X-Frame:  %s\n",
                std::string(res.get_header("X-Frame-Options")).c_str());
    std::printf("  X-CTO:    %s\n",
                std::string(res.get_header("X-Content-Type-Options")).c_str());
    std::printf("  Referrer: %s\n",
                std::string(res.get_header("Referrer-Policy")).c_str());
    std::printf("  Perms:    %s\n",
                std::string(res.get_header("Permissions-Policy")).c_str());

    // 통합 secure_headers() — 모든 보안 헤더 일괄 적용
    Response res2;
    secure_headers()(req, res2);
    bool has_hsts = !res2.get_header("Strict-Transport-Security").empty();
    std::printf("  secure_headers(): HSTS 포함 = %s\n\n",
                has_hsts ? "yes" : "no");
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  Bearer 토큰 인증 (token_auth.hpp / jwt.hpp shim)
// ─────────────────────────────────────────────────────────────────────────────

static void demo_bearer_auth() {
    std::printf("── §2  Bearer 토큰 인증 ──\n");

    // 커스텀 토큰 검증기 구현
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

    // 유효한 토큰
    // On success, bearer_auth() forwards claims into res headers:
    //   X-Auth-Sub  → claims.subject
    {
        Request req;
        req.add_header("Authorization", "Bearer valid-token-abc123");
        Response res;
        bool ok = auth_mw(req, res);
        std::printf("  유효 토큰: %s (sub=%s)\n",
                    ok ? "통과" : "차단",
                    ok ? std::string(res.get_header("X-Auth-Sub")).c_str() : "-");
    }

    // 무효 토큰
    {
        Request req;
        req.add_header("Authorization", "Bearer wrong-token");
        Response res;
        bool ok = auth_mw(req, res);
        std::printf("  무효 토큰: %s (status=%d)\n",
                    ok ? "통과" : "차단", res.status_code());
    }

    // 헤더 없음
    {
        Request req;
        Response res;
        bool ok = auth_mw(req, res);
        std::printf("  헤더 없음: %s\n\n", ok ? "통과" : "차단");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// §3  API Key 인증
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
    std::printf("── §3  API Key 인증 ──\n");

    // 허용된 API 키 집합
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
        std::printf("  key='%s': %s %s\n",
                    tc.key, ok ? "통과" : "차단",
                    (ok == tc.expect_pass) ? "OK" : "FAIL");
    }
    std::printf("\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// §4  정적 파일 서빙 유틸리티
// ─────────────────────────────────────────────────────────────────────────────

static void demo_static_files() {
    std::printf("── §4  정적 파일 유틸리티 ──\n");

    // MIME 타입 감지
    struct MimeCase { const char* ext; };
    std::vector<MimeCase> mimes = {
        {".html"}, {".css"}, {".js"}, {".json"},
        {".png"},  {".svg"}, {".wasm"}, {".unknown"},
    };

    for (auto& m : mimes) {
        std::printf("  mime_type(\"%s\") = %s\n",
                    m.ext, std::string(mime_type(m.ext)).c_str());
    }

    // serve_file(): 파일 존재 여부에 따라 응답 생성
    // Signature: serve_file(string_view fs_path, Response& res) -> void
    Response res;
    // 존재하지 않는 파일 경로 → 404 응답 기대
    serve_file("/tmp/nonexistent_file_qbuem.html", res);
    std::printf("  serve_file(없는파일): status=%d\n\n", res.status_code());
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
    std::printf("── §5  Body Encoder ──\n");

    // IdentityEncoder — 변환 없는 패스스루
    IdentityEncoder identity;
    std::string body = "Hello, qbuem body encoder!";
    std::string encoded;
    bool ok = identity.encode(body, encoded);
    std::printf("  IdentityEncoder: in=%zu out=%zu same=%s\n",
                body.size(), encoded.size(),
                (ok && body == encoded) ? "yes" : "no");
    std::printf("  content-encoding: %s\n", identity.encoding_name().data());

    // compress() middleware: returned Middleware always returns true (pre-handler stub).
    // compress_response() is the in-handler post-processing helper.
    auto mw = compress(identity);

    Request req;
    Response res;
    res.body(body);
    mw(req, res);
    std::printf("  compress() 미들웨어 적용 후 body 크기: %zu\n\n",
                res.get_body().size());
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::printf("=== qbuem 보안 미들웨어 예제 ===\n\n");

    demo_security_headers();
    demo_bearer_auth();
    demo_api_key();
    demo_static_files();
    demo_body_encoder();

    std::printf("=== 완료 ===\n");
    return 0;
}
