// middleware_coverage_test.cpp
//
// Coverage for include/qbuem/middleware/* beyond the existing
// http_middleware / middleware_extra suites. Targets the standalone
// middleware factories and helpers:
//
//   cors.hpp          — preflight 204 + CORS headers + dynamic whitelist
//   rate_limit.hpp    — token-bucket allow / deny / refill / per-key override
//   content_type.hpp  — require_json / require_content_type (415)
//   sse.hpp           — SSE event formatting (data / event / id / retry / multiline)
//   static_files.hpp  — mime_type mapping + file_extension + serve_file (/tmp)
//   body_encoder.hpp  — IBodyEncoder interface + compress_response gating
//   token_auth.hpp    — bearer extraction (shared_ptr + ref overloads)
//   security.hpp      — security headers (hsts/csp/xfo/nosniff/secure_headers)
//   request_id.hpp    — UUID v4 shape + echo of incoming ID
//
// All tests are deterministic, single-process, no real sockets / no sleeps.
// Errors in this library surface as state on the Response (status code +
// halt bool), not exceptions, so every test inspects the Response state and
// the middleware return value (true = continue chain, false = halt).

#include <gtest/gtest.h>

#include <qbuem/http/request.hpp>
#include <qbuem/http/response.hpp>
#include <qbuem/http/router.hpp>

#include <qbuem/middleware/cors.hpp>
#include <qbuem/middleware/rate_limit.hpp>
#include <qbuem/middleware/content_type.hpp>
#include <qbuem/middleware/sse.hpp>
#include <qbuem/middleware/static_files.hpp>
#include <qbuem/middleware/body_encoder.hpp>
#include <qbuem/middleware/token_auth.hpp>
#include <qbuem/middleware/security.hpp>
#include <qbuem/middleware/request_id.hpp>

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

using qbuem::Method;
using qbuem::Request;
using qbuem::Response;
using qbuem::Middleware;
namespace mw = qbuem::middleware;

// ───────────────────────── Test helpers ──────────────────────────

namespace {

// Build a Request with method + path. Header strings passed to add_header must
// outlive the Request (they are stored as string_views), so callers keep the
// backing std::string alive in their own scope.
Request make_request(Method m, std::string_view path = "/") {
  Request req;
  req.set_method(m);
  req.set_path(path);
  return req;
}

} // namespace

// ════════════════════════════ CORS ═══════════════════════════════

TEST(CorsMiddleware, AddsHeadersOnNormalRequest) {
  Middleware m = mw::cors();
  Request req = make_request(Method::Get, "/api");
  Response res;

  bool cont = m(req, res);
  EXPECT_TRUE(cont); // non-OPTIONS continues the chain
  EXPECT_EQ(res.get_header("Access-Control-Allow-Origin"), "*");
  EXPECT_FALSE(res.get_header("Access-Control-Allow-Methods").empty());
  EXPECT_FALSE(res.get_header("Access-Control-Allow-Headers").empty());
}

TEST(CorsMiddleware, PreflightOptionsHalts204) {
  Middleware m = mw::cors();
  Request req = make_request(Method::Options, "/api");
  Response res;

  bool cont = m(req, res);
  EXPECT_FALSE(cont); // OPTIONS preflight halts the chain
  EXPECT_EQ(res.status_code(), 204);
  EXPECT_FALSE(res.get_header("Access-Control-Max-Age").empty());
}

TEST(CorsMiddleware, CredentialsHeaderEmitted) {
  mw::CorsConfig cfg;
  cfg.allow_origin = "https://example.com";
  cfg.allow_credentials = true;
  Middleware m = mw::cors(cfg);

  Request req = make_request(Method::Get, "/x");
  Response res;
  EXPECT_TRUE(m(req, res));
  EXPECT_EQ(res.get_header("Access-Control-Allow-Origin"), "https://example.com");
  EXPECT_EQ(res.get_header("Access-Control-Allow-Credentials"), "true");
}

TEST(CorsMiddleware, DynamicWhitelistReflectsMatchingOrigin) {
  mw::CorsConfig cfg;
  cfg.allow_origins = {"https://app.example.com", "https://admin.example.com"};
  Middleware m = mw::cors(cfg);

  // Matching origin → reflected + Vary header.
  std::string origin_hdr = "Origin";
  std::string origin_val = "https://admin.example.com";
  Request req = make_request(Method::Get, "/x");
  req.add_header(origin_hdr, origin_val);
  Response res;
  EXPECT_TRUE(m(req, res));
  EXPECT_EQ(res.get_header("Access-Control-Allow-Origin"), origin_val);
  EXPECT_EQ(res.get_header("Vary"), "Origin");
}

TEST(CorsMiddleware, DynamicWhitelistRejectsUnknownOrigin) {
  mw::CorsConfig cfg;
  cfg.allow_origins = {"https://app.example.com"};
  Middleware m = mw::cors(cfg);

  std::string origin_hdr = "Origin";
  std::string origin_val = "https://evil.example.com";
  Request req = make_request(Method::Get, "/x");
  req.add_header(origin_hdr, origin_val);
  Response res;
  // Not whitelisted: no CORS header, chain continues.
  EXPECT_TRUE(m(req, res));
  EXPECT_TRUE(res.get_header("Access-Control-Allow-Origin").empty());
}

// ═════════════════════════ Rate limit ════════════════════════════

TEST(RateLimitMiddleware, AllowsWithinBurstThenDenies) {
  // burst=3 so the 4th request in the same instant exhausts the bucket.
  mw::RateLimitConfig cfg;
  cfg.rate_per_sec = 0.0; // no refill within the test window
  cfg.burst = 3.0;
  cfg.key_fn = [](const Request &) -> std::string { return "fixed-key-allow-deny"; };
  Middleware m = mw::rate_limit(cfg);

  int allowed = 0;
  int denied = 0;
  for (int i = 0; i < 5; ++i) {
    Request req = make_request(Method::Get, "/x");
    Response res;
    if (m(req, res)) {
      ++allowed;
    } else {
      ++denied;
      EXPECT_EQ(res.status_code(), 429);
      EXPECT_FALSE(res.get_header("Retry-After").empty());
    }
  }
  EXPECT_EQ(allowed, 3); // exactly burst tokens granted
  EXPECT_EQ(denied, 2);
}

TEST(RateLimitMiddleware, EmitsRateLimitHeaders) {
  mw::RateLimitConfig cfg;
  cfg.rate_per_sec = 100.0;
  cfg.burst = 10.0;
  cfg.key_fn = [](const Request &) -> std::string { return "fixed-key-headers"; };
  Middleware m = mw::rate_limit(cfg);

  Request req = make_request(Method::Get, "/x");
  Response res;
  EXPECT_TRUE(m(req, res));
  EXPECT_EQ(res.get_header("X-RateLimit-Limit"), "10");
  // First call consumes one token: remaining reported as limit-1.
  EXPECT_EQ(res.get_header("X-RateLimit-Remaining"), "9");
}

TEST(RateLimitMiddleware, PerKeyOverrideRaisesBurst) {
  mw::RateLimitConfig cfg;
  cfg.rate_per_sec = 0.0;
  cfg.burst = 1.0; // global is very tight
  cfg.key_fn = [](const Request &) -> std::string { return "vip-key"; };
  cfg.per_key_override =
      [](const std::string &key) -> std::optional<std::pair<double, double>> {
    if (key == "vip-key") return std::make_pair(0.0, 5.0); // burst=5 for VIP
    return std::nullopt;
  };
  Middleware m = mw::rate_limit(cfg);

  int allowed = 0;
  for (int i = 0; i < 5; ++i) {
    Request req = make_request(Method::Get, "/x");
    Response res;
    if (m(req, res)) ++allowed;
  }
  EXPECT_EQ(allowed, 5); // override burst applied, not the global burst=1
}

TEST(RateLimitMiddleware, DefaultKeyFnUsesGlobalBucket) {
  // No key_fn provided → falls back to "__global__"; just verify it runs and
  // produces the standard headers without crashing.
  mw::RateLimitConfig cfg;
  cfg.rate_per_sec = 100.0;
  cfg.burst = 50.0;
  Middleware m = mw::rate_limit(cfg);

  Request req = make_request(Method::Get, "/x");
  Response res;
  EXPECT_TRUE(m(req, res));
  EXPECT_FALSE(res.get_header("X-RateLimit-Limit").empty());
}

// ═════════════════════════ Content-Type ══════════════════════════

TEST(ContentTypeMiddleware, RequireJsonRejectsMissingType) {
  Middleware m = mw::require_json();
  Request req = make_request(Method::Post, "/data"); // no Content-Type
  Response res;
  EXPECT_FALSE(m(req, res)); // halt
  EXPECT_EQ(res.status_code(), 415);
}

TEST(ContentTypeMiddleware, RequireJsonAcceptsJsonType) {
  Middleware m = mw::require_json();
  std::string ct_key = "Content-Type";
  std::string ct_val = "application/json; charset=utf-8";
  Request req = make_request(Method::Post, "/data");
  req.add_header(ct_key, ct_val);
  Response res;
  EXPECT_TRUE(m(req, res)); // substring match → continue
  EXPECT_EQ(res.status_code(), 200); // default, untouched
}

TEST(ContentTypeMiddleware, RequireJsonIgnoresGetRequests) {
  // GET is not in the default {Post, Put, Patch} set → passes through.
  Middleware m = mw::require_json();
  Request req = make_request(Method::Get, "/data");
  Response res;
  EXPECT_TRUE(m(req, res));
}

TEST(ContentTypeMiddleware, CustomTypeAndMethodSet) {
  Middleware m = mw::require_content_type("multipart/form-data", {Method::Post});

  // POST without the type → rejected.
  {
    Request req = make_request(Method::Post, "/upload");
    Response res;
    EXPECT_FALSE(m(req, res));
    EXPECT_EQ(res.status_code(), 415);
  }
  // PUT is not in the enforced set → passes through.
  {
    Request req = make_request(Method::Put, "/upload");
    Response res;
    EXPECT_TRUE(m(req, res));
  }
}

// ═══════════════════════════ SSE ═════════════════════════════════

TEST(SseStream, DataOnlyFrame) {
  Response res;
  {
    qbuem::SseStream sse(res);
    sse.send("hello");
  } // destructor calls close() → end_chunks()
  EXPECT_TRUE(res.is_chunked());
  std::string buf(res.chunk_buf());
  EXPECT_NE(buf.find("data: hello\n"), std::string::npos);
  // event/id/retry fields absent
  EXPECT_EQ(buf.find("event: "), std::string::npos);
}

TEST(SseStream, EventIdRetryFields) {
  Response res;
  {
    qbuem::SseStream sse(res);
    sse.send("42", "counter", "7", 3000);
  }
  std::string buf(res.chunk_buf());
  EXPECT_NE(buf.find("event: counter\n"), std::string::npos);
  EXPECT_NE(buf.find("id: 7\n"), std::string::npos);
  EXPECT_NE(buf.find("retry: 3000\n"), std::string::npos);
  EXPECT_NE(buf.find("data: 42\n"), std::string::npos);
}

TEST(SseStream, MultiLineDataSplitsIntoSeparateFields) {
  Response res;
  {
    qbuem::SseStream sse(res);
    sse.send("line1\nline2\nline3");
  }
  std::string buf(res.chunk_buf());
  EXPECT_NE(buf.find("data: line1\n"), std::string::npos);
  EXPECT_NE(buf.find("data: line2\n"), std::string::npos);
  EXPECT_NE(buf.find("data: line3\n"), std::string::npos);
}

TEST(SseStream, HeartbeatAndContentType) {
  Response res;
  {
    qbuem::SseStream sse(res);
    sse.heartbeat();
  }
  EXPECT_EQ(res.status_code(), 200);
  EXPECT_NE(res.get_header("Content-Type").find("text/event-stream"),
            std::string_view::npos);
  EXPECT_EQ(res.get_header("Cache-Control"), "no-cache");
  std::string buf(res.chunk_buf());
  EXPECT_NE(buf.find(": ping\n\n"), std::string::npos);
}

TEST(SseStream, DoubleCloseIsIdempotent) {
  Response res;
  qbuem::SseStream sse(res);
  sse.send("x");
  sse.close();
  sse.close(); // second close must be a no-op, not a crash
  EXPECT_TRUE(res.is_chunked());
}

// ════════════════════════ Static files ═══════════════════════════

TEST(StaticFiles, MimeTypeKnownExtensions) {
  EXPECT_EQ(mw::mime_type(".html"), "text/html; charset=utf-8");
  EXPECT_EQ(mw::mime_type(".css"), "text/css; charset=utf-8");
  EXPECT_EQ(mw::mime_type(".js"), "text/javascript; charset=utf-8");
  EXPECT_EQ(mw::mime_type(".json"), "application/json");
  EXPECT_EQ(mw::mime_type(".png"), "image/png");
  EXPECT_EQ(mw::mime_type(".svg"), "image/svg+xml");
  EXPECT_EQ(mw::mime_type(".woff2"), "font/woff2");
  EXPECT_EQ(mw::mime_type(".wasm"), "application/wasm");
}

TEST(StaticFiles, MimeTypeUnknownFallsBackToOctetStream) {
  EXPECT_EQ(mw::mime_type(".xyz"), "application/octet-stream");
  EXPECT_EQ(mw::mime_type(""), "application/octet-stream");
}

TEST(StaticFiles, FileExtensionParsing) {
  EXPECT_EQ(mw::file_extension("/foo/bar.js"), ".js");
  EXPECT_EQ(mw::file_extension("index.html"), ".html");
  // A dotted directory but no file extension → empty.
  EXPECT_TRUE(mw::file_extension("/foo.bar/baz").empty());
  // No dot at all → empty.
  EXPECT_TRUE(mw::file_extension("/foo/bar").empty());
}

TEST(StaticFiles, ServeExistingTempFile) {
  // Write a temp .txt file, serve it, then clean up.
  const char *path = "/tmp/qbuem_mw_cov_serve.txt";
  const std::string contents = "static-file-body-content";
  {
    std::ofstream out(path, std::ios::binary);
    ASSERT_TRUE(out.good());
    out << contents;
  }

  Response res;
  mw::serve_file(path, res);
  EXPECT_EQ(res.status_code(), 200);
  EXPECT_EQ(res.get_header("Content-Type"), "text/plain; charset=utf-8");
  EXPECT_FALSE(res.get_header("ETag").empty());
  EXPECT_FALSE(res.get_header("Last-Modified").empty());

#ifdef __linux__
  // Linux uses the zero-copy sendfile() path: body stays empty, path stored.
  EXPECT_TRUE(res.has_sendfile());
  EXPECT_EQ(res.sendfile_size(), contents.size());
#else
  // macOS / portable fallback reads the file into the body.
  EXPECT_EQ(res.get_body(), contents);
#endif

  std::remove(path);
}

TEST(StaticFiles, ServeMissingFileReturns404) {
  Response res;
  mw::serve_file("/tmp/qbuem_mw_cov_definitely_missing_3f8a.txt", res);
  EXPECT_EQ(res.status_code(), 404);
  EXPECT_EQ(res.get_body(), "Not Found");
}

TEST(StaticFiles, PathTraversalStyleMissingTargetReturns404) {
  // static_files.hpp does not own path sanitization (that lives in the App
  // layer). What it DOES guarantee is graceful 404 for a traversal-style path
  // that does not resolve to a regular file — no crash, no leak.
  Response res;
  mw::serve_file("/tmp/qbuem_mw_cov_dir/../../etc/qbuem_no_such_file", res);
  EXPECT_EQ(res.status_code(), 404);
}

TEST(StaticFiles, ServeDirectoryIsRejectedAs404) {
  // A directory is not a regular file → 404 (S_ISREG guard).
  Response res;
  mw::serve_file("/tmp", res);
  EXPECT_EQ(res.status_code(), 404);
}

// ════════════════════════ Body encoder ═══════════════════════════

namespace {

// Trivial reversible "encoder" (uppercases) to exercise the IBodyEncoder
// interface and compress_response gating without any external library.
class UpperEncoder final : public mw::IBodyEncoder {
public:
  bool encode(std::string_view src, std::string &dst) noexcept override {
    dst.assign(src.begin(), src.end());
    for (char &c : dst)
      if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return true;
  }
  std::string_view encoding_name() const noexcept override { return "x-upper"; }
  std::string_view accept_token() const noexcept override { return "x-upper"; }
};

} // namespace

TEST(BodyEncoder, InterfaceTokens) {
  UpperEncoder enc;
  EXPECT_EQ(enc.encoding_name(), "x-upper");
  EXPECT_EQ(enc.accept_token(), "x-upper");
  std::string out;
  EXPECT_TRUE(enc.encode("abc", out));
  EXPECT_EQ(out, "ABC");
}

TEST(BodyEncoder, CompressResponseAppliesWhenEligible) {
  UpperEncoder enc;

  std::string ae_key = "Accept-Encoding";
  std::string ae_val = "gzip, x-upper";
  Request req = make_request(Method::Get, "/x");
  req.add_header(ae_key, ae_val);

  Response res;
  // Compressible content type + body >= min_size.
  std::string body(300, 'a');
  res.status(200).header("Content-Type", "text/plain").body(body);

  mw::compress_response(enc, req, res, 256);
  EXPECT_EQ(res.get_header("Content-Encoding"), "x-upper");
  EXPECT_EQ(res.get_header("Vary"), "Accept-Encoding");
  EXPECT_EQ(res.get_body(), std::string(300, 'A')); // uppercased by encoder
}

TEST(BodyEncoder, CompressResponseSkipsBelowMinSize) {
  UpperEncoder enc;
  std::string ae_key = "Accept-Encoding";
  std::string ae_val = "x-upper";
  Request req = make_request(Method::Get, "/x");
  req.add_header(ae_key, ae_val);

  Response res;
  res.status(200).header("Content-Type", "text/plain").body("tiny");
  mw::compress_response(enc, req, res, 256); // body smaller than min_size
  EXPECT_TRUE(res.get_header("Content-Encoding").empty());
  EXPECT_EQ(res.get_body(), "tiny"); // unchanged
}

TEST(BodyEncoder, CompressResponseSkipsNonCompressibleType) {
  UpperEncoder enc;
  std::string ae_key = "Accept-Encoding";
  std::string ae_val = "x-upper";
  Request req = make_request(Method::Get, "/x");
  req.add_header(ae_key, ae_val);

  Response res;
  std::string body(300, 'a');
  res.status(200).header("Content-Type", "image/png").body(body); // not compressible
  mw::compress_response(enc, req, res, 256);
  EXPECT_TRUE(res.get_header("Content-Encoding").empty());
}

TEST(BodyEncoder, CompressResponseSkipsWhenClientDoesNotAccept) {
  UpperEncoder enc;
  std::string ae_key = "Accept-Encoding";
  std::string ae_val = "gzip"; // does not include x-upper
  Request req = make_request(Method::Get, "/x");
  req.add_header(ae_key, ae_val);

  Response res;
  std::string body(300, 'a');
  res.status(200).header("Content-Type", "application/json").body(body);
  mw::compress_response(enc, req, res, 256);
  EXPECT_TRUE(res.get_header("Content-Encoding").empty());
}

// ════════════════════════ Token auth ═════════════════════════════

namespace {

// Accepts only the literal token "valid-token"; rejects everything else.
class FixedVerifier final : public mw::ITokenVerifier {
public:
  std::optional<mw::TokenClaims> verify(std::string_view token) noexcept override {
    if (token == "valid-token") {
      mw::TokenClaims c;
      c.subject = "user-42";
      c.issuer = "qbuem-test";
      c.audience = "test-aud";
      c.custom["role"] = "admin";
      return c;
    }
    return std::nullopt;
  }
};

} // namespace

TEST(TokenAuth, MissingBearerReturns401) {
  auto v = std::make_shared<FixedVerifier>();
  Middleware m = mw::bearer_auth(v);
  Request req = make_request(Method::Get, "/me"); // no Authorization header
  Response res;
  EXPECT_FALSE(m(req, res));
  EXPECT_EQ(res.status_code(), 401);
  EXPECT_FALSE(res.get_header("WWW-Authenticate").empty());
}

TEST(TokenAuth, InvalidTokenReturns401) {
  auto v = std::make_shared<FixedVerifier>();
  Middleware m = mw::bearer_auth(v);
  std::string auth_key = "Authorization";
  std::string auth_val = "Bearer wrong-token";
  Request req = make_request(Method::Get, "/me");
  req.add_header(auth_key, auth_val);
  Response res;
  EXPECT_FALSE(m(req, res));
  EXPECT_EQ(res.status_code(), 401);
  EXPECT_NE(res.get_header("WWW-Authenticate").find("invalid_token"),
            std::string_view::npos);
}

TEST(TokenAuth, ValidTokenForwardsClaimsAsHeaders) {
  auto v = std::make_shared<FixedVerifier>();
  Middleware m = mw::bearer_auth(v);
  std::string auth_key = "Authorization";
  std::string auth_val = "Bearer valid-token";
  Request req = make_request(Method::Get, "/me");
  req.add_header(auth_key, auth_val);
  Response res;
  EXPECT_TRUE(m(req, res)); // continue
  EXPECT_EQ(res.get_header("X-Auth-Sub"), "user-42");
  EXPECT_EQ(res.get_header("X-Auth-Iss"), "qbuem-test");
  EXPECT_EQ(res.get_header("X-Auth-Aud"), "test-aud");
  EXPECT_EQ(res.get_header("X-Auth-role"), "admin"); // custom claim forwarded
}

TEST(TokenAuth, ReferenceOverloadWorks) {
  FixedVerifier v;
  Middleware m = mw::bearer_auth(v); // reference overload
  std::string auth_key = "Authorization";
  std::string auth_val = "Bearer valid-token";
  Request req = make_request(Method::Get, "/me");
  req.add_header(auth_key, auth_val);
  Response res;
  EXPECT_TRUE(m(req, res));
  EXPECT_EQ(res.get_header("X-Auth-Sub"), "user-42");
}

TEST(TokenAuth, BearerPrefixWithoutTokenRejected) {
  auto v = std::make_shared<FixedVerifier>();
  Middleware m = mw::bearer_auth(v);
  std::string auth_key = "Authorization";
  std::string auth_val = "Bearer"; // no trailing space + token
  Request req = make_request(Method::Get, "/me");
  req.add_header(auth_key, auth_val);
  Response res;
  EXPECT_FALSE(m(req, res));
  EXPECT_EQ(res.status_code(), 401);
}

TEST(TokenAuth, CustomOnErrorHandlerInvoked) {
  auto v = std::make_shared<FixedVerifier>();
  mw::BearerAuthOptions opts;
  bool called = false;
  opts.on_error = [&called](const Request &, Response &res, std::string_view) {
    called = true;
    res.status(403).body("custom-denied");
  };
  Middleware m = mw::bearer_auth(v, opts);

  Request req = make_request(Method::Get, "/me"); // no Authorization
  Response res;
  EXPECT_FALSE(m(req, res));
  EXPECT_TRUE(called);
  EXPECT_EQ(res.status_code(), 403);
  EXPECT_EQ(res.get_body(), "custom-denied");
}

// ════════════════════════ Security headers ═══════════════════════

TEST(SecurityHeaders, HstsDefault) {
  Middleware m = mw::hsts();
  Request req = make_request(Method::Get, "/x");
  Response res;
  EXPECT_TRUE(m(req, res));
  std::string v(res.get_header("Strict-Transport-Security"));
  EXPECT_NE(v.find("max-age=31536000"), std::string::npos);
  EXPECT_NE(v.find("includeSubDomains"), std::string::npos);
  EXPECT_EQ(v.find("preload"), std::string::npos); // default off
}

TEST(SecurityHeaders, HstsWithPreload) {
  Middleware m = mw::hsts(600, false, true);
  Request req = make_request(Method::Get, "/x");
  Response res;
  EXPECT_TRUE(m(req, res));
  std::string v(res.get_header("Strict-Transport-Security"));
  EXPECT_NE(v.find("max-age=600"), std::string::npos);
  EXPECT_EQ(v.find("includeSubDomains"), std::string::npos);
  EXPECT_NE(v.find("preload"), std::string::npos);
}

TEST(SecurityHeaders, CspAndFrameAndNosniff) {
  Request req = make_request(Method::Get, "/x");
  {
    Response res;
    EXPECT_TRUE(mw::csp("default-src 'self'")(req, res));
    EXPECT_EQ(res.get_header("Content-Security-Policy"), "default-src 'self'");
  }
  {
    Response res;
    EXPECT_TRUE(mw::x_frame_options("DENY")(req, res));
    EXPECT_EQ(res.get_header("X-Frame-Options"), "DENY");
  }
  {
    Response res;
    EXPECT_TRUE(mw::x_frame_options()(req, res)); // default SAMEORIGIN
    EXPECT_EQ(res.get_header("X-Frame-Options"), "SAMEORIGIN");
  }
  {
    Response res;
    EXPECT_TRUE(mw::x_content_type_options()(req, res));
    EXPECT_EQ(res.get_header("X-Content-Type-Options"), "nosniff");
  }
  {
    Response res;
    EXPECT_TRUE(mw::referrer_policy()(req, res));
    EXPECT_EQ(res.get_header("Referrer-Policy"), "strict-origin-when-cross-origin");
  }
  {
    Response res;
    EXPECT_TRUE(mw::permissions_policy("camera=()")(req, res));
    EXPECT_EQ(res.get_header("Permissions-Policy"), "camera=()");
  }
}

TEST(SecurityHeaders, SecureHeadersBundleDefault) {
  Middleware m = mw::secure_headers();
  Request req = make_request(Method::Get, "/x");
  Response res;
  EXPECT_TRUE(m(req, res));
  EXPECT_FALSE(res.get_header("Strict-Transport-Security").empty());
  EXPECT_EQ(res.get_header("Content-Security-Policy"), "default-src 'self'");
  EXPECT_EQ(res.get_header("X-Frame-Options"), "SAMEORIGIN");
  EXPECT_EQ(res.get_header("X-Content-Type-Options"), "nosniff");
  EXPECT_EQ(res.get_header("Referrer-Policy"), "strict-origin-when-cross-origin");
  // perms_policy empty by default → header omitted.
  EXPECT_TRUE(res.get_header("Permissions-Policy").empty());
}

TEST(SecurityHeaders, SecureHeadersBundleHstsDisabledAndPermsSet) {
  mw::SecureHeadersConfig cfg;
  cfg.hsts_enabled = false;
  cfg.perms_policy = "geolocation=()";
  Middleware m = mw::secure_headers(cfg);
  Request req = make_request(Method::Get, "/x");
  Response res;
  EXPECT_TRUE(m(req, res));
  EXPECT_TRUE(res.get_header("Strict-Transport-Security").empty()); // disabled
  EXPECT_EQ(res.get_header("Permissions-Policy"), "geolocation=()");
}

// ═══════════════════════════ Request ID ══════════════════════════

namespace {

// Validate UUID v4 textual shape: 8-4-4-4-12 hex with version '4' and variant.
bool is_uuid_v4_shape(std::string_view s) {
  if (s.size() != 36) return false;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else {
      bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      if (!hex) return false;
    }
  }
  if (s[14] != '4') return false; // version nibble
  char var = s[19];               // variant nibble must be one of 8,9,a,b
  return var == '8' || var == '9' || var == 'a' || var == 'b';
}

} // namespace

TEST(RequestId, GeneratesUuidV4WhenAbsent) {
  Middleware m = mw::request_id();
  Request req = make_request(Method::Get, "/x");
  Response res;
  EXPECT_TRUE(m(req, res));
  std::string id(res.get_header("X-Request-ID"));
  EXPECT_TRUE(is_uuid_v4_shape(id)) << "got: " << id;
}

TEST(RequestId, GeneratedIdsAreUnique) {
  Middleware m = mw::request_id();
  Request req1 = make_request(Method::Get, "/a");
  Request req2 = make_request(Method::Get, "/b");
  Response res1, res2;
  EXPECT_TRUE(m(req1, res1));
  EXPECT_TRUE(m(req2, res2));
  EXPECT_NE(res1.get_header("X-Request-ID"), res2.get_header("X-Request-ID"));
}

TEST(RequestId, EchoesIncomingHeader) {
  Middleware m = mw::request_id();
  std::string key = "X-Request-ID";
  std::string val = "incoming-id-12345";
  Request req = make_request(Method::Get, "/x");
  req.add_header(key, val);
  Response res;
  EXPECT_TRUE(m(req, res));
  EXPECT_EQ(res.get_header("X-Request-ID"), val); // echoed, not regenerated
}

TEST(RequestId, CustomHeaderName) {
  Middleware m = mw::request_id("X-Trace-ID");
  Request req = make_request(Method::Get, "/x");
  Response res;
  EXPECT_TRUE(m(req, res));
  EXPECT_TRUE(is_uuid_v4_shape(res.get_header("X-Trace-ID")));
  EXPECT_TRUE(res.get_header("X-Request-ID").empty()); // custom name only
}
