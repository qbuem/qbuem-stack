#include <qbuem/crypto.hpp>
#include <qbuem/qbuem.hpp>
#include <qbuem/http/parser.hpp>
#include <qbuem/http/router.hpp>
#include <qbuem/middleware/rate_limit.hpp>
#include <qbuem/middleware/request_id.hpp>
#include <qbuem/middleware/security.hpp>
#include <qbuem/middleware/static_files.hpp>
#include <gtest/gtest.h>

using namespace qbuem;

TEST(HttpParserTest, BasicParse) {
  HttpParser parser;
  Request req;
  std::string_view raw = "GET /index.html HTTP/1.1\r\nHost: "
                         "localhost\r\nContent-Length: 5\r\n\r\nhello";

  auto result = parser.parse(raw, req);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(req.method(), Method::Get);
  EXPECT_EQ(req.path(), "/index.html");
  EXPECT_EQ(req.header("Host"), "localhost");
  EXPECT_EQ(req.body(), "hello");
}

TEST(HttpParserTest, ChunkedBody) {
  // Chunked transfer: "hello" (5) + " world" (6) + last chunk (0)
  std::string raw =
      "POST /upload HTTP/1.1\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "5\r\nhello\r\n"
      "6\r\n world\r\n"
      "0\r\n"
      "\r\n";

  HttpParser parser;
  Request req;
  auto result = parser.parse(raw, req);
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(parser.is_complete());
  EXPECT_EQ(req.body(), "hello world");
}

TEST(HttpParserTest, ChunkedPartialBody) {
  // Arrive in two parts; second call re-parses the full buffer
  std::string part1 =
      "POST /up HTTP/1.1\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "5\r\nhel";   // chunk data split

  HttpParser p1;
  Request r1;
  auto res1 = p1.parse(part1, r1);
  ASSERT_TRUE(res1.has_value());  // partial → returns bytes consumed so far
  EXPECT_FALSE(p1.is_complete());

  // Full buffer now
  std::string full = part1 + "lo\r\n0\r\n\r\n";
  HttpParser p2;
  Request r2;
  auto res2 = p2.parse(full, r2);
  ASSERT_TRUE(res2.has_value());
  EXPECT_TRUE(p2.is_complete());
  EXPECT_EQ(r2.body(), "hello");
}

TEST(HttpParserTest, ChunkedWithExtension) {
  // Chunk extensions ("; name=value") must be silently ignored
  std::string raw =
      "POST / HTTP/1.1\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "3;ext=val\r\nabc\r\n"
      "0\r\n"
      "\r\n";

  HttpParser parser;
  Request req;
  auto result = parser.parse(raw, req);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(parser.is_complete());
  EXPECT_EQ(req.body(), "abc");
}

TEST(HttpParserTest, PayloadTooLarge) {
  // Content-Length larger than MAX_BODY_SIZE → error_status 413
  std::string raw =
      "POST /big HTTP/1.1\r\n"
      "Content-Length: 2097152\r\n"  // 2 MiB > 1 MiB limit
      "\r\n";

  HttpParser parser;
  Request req;
  auto result = parser.parse(raw, req);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(parser.error_status(), 413);
}

TEST(HttpParserTest, HeaderInjectionRejected) {
  // A header value containing a bare \r (CR without immediately following LF
  // as the terminator) must be rejected with status 400.
  // Attack pattern: "X-Evil: foo\r bar" — the embedded CR could confuse
  // downstream code that processes header values.
  std::string raw =
      "GET / HTTP/1.1\r\n"
      "X-Evil: foo\r bar\r\n"   // bare \r embedded mid-value
      "\r\n";

  HttpParser parser;
  Request req;
  auto result = parser.parse(raw, req);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(parser.error_status(), 400);
}

TEST(HttpParserTest, HeadersCompleteBeforeBody) {
  // With Content-Length but no body data yet, headers_complete() == true
  std::string raw =
      "POST /data HTTP/1.1\r\n"
      "Content-Length: 10\r\n"
      "\r\n";  // body not included

  HttpParser parser;
  Request req;
  auto result = parser.parse(raw, req);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(parser.headers_complete());
  EXPECT_FALSE(parser.is_complete());
}

TEST(HttpParserTest, QueryStringSplit) {
  std::string raw =
      "GET /search?q=hello&page=2 HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "\r\n";

  HttpParser parser;
  Request req;
  auto result = parser.parse(raw, req);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(req.path(), "/search");
  EXPECT_EQ(req.query_string(), "q=hello&page=2");
  EXPECT_EQ(req.query("q"), "hello");
  EXPECT_EQ(req.query("page"), "2");
  EXPECT_EQ(req.query("missing"), "");
}

TEST(HttpParserTest, RequestSmugglingRejected) {
  // Both Transfer-Encoding and Content-Length → 400
  std::string raw =
      "POST /data HTTP/1.1\r\n"
      "Content-Length: 5\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n";

  HttpParser parser;
  Request req;
  auto result = parser.parse(raw, req);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(parser.error_status(), 400);
}

TEST(HttpParserTest, CookieParsing) {
  std::string raw =
      "GET /profile HTTP/1.1\r\n"
      "Cookie: session=abc123; theme=dark; lang=ko\r\n"
      "\r\n";

  HttpParser parser;
  Request req;
  auto result = parser.parse(raw, req);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(req.cookie("session"), "abc123");
  EXPECT_EQ(req.cookie("theme"),   "dark");
  EXPECT_EQ(req.cookie("lang"),    "ko");
  EXPECT_EQ(req.cookie("missing"), "");
}

TEST(HttpParserTest, FormBodyParsing) {
  std::string raw =
      "POST /login HTTP/1.1\r\n"
      "Content-Type: application/x-www-form-urlencoded\r\n"
      "Content-Length: 28\r\n"
      "\r\n"
      "username=alice&password=s3cr";  // 28 bytes

  HttpParser parser;
  Request req;
  auto result = parser.parse(raw, req);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(req.form("username"), "alice");
  EXPECT_EQ(req.form("password"), "s3cr");
  EXPECT_EQ(req.form("missing"),  "");

  // form() returns empty for wrong Content-Type
  std::string raw2 =
      "POST /data HTTP/1.1\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: 2\r\n"
      "\r\n"
      "{}";
  HttpParser p2;
  Request r2;
  p2.parse(raw2, r2);
  EXPECT_EQ(r2.form("key"), "");
}

TEST(ResponseTest, SetCookieBuilder) {
  Response res;
  res.status(200)
     .set_cookie("session", "tok123",
                 {.path = "/", .domain = "", .same_site = "Lax",
                  .max_age = 3600, .http_only = true, .secure = true})
     .set_cookie("pref", "dark");

  std::string raw = res.serialize();
  EXPECT_NE(raw.find("Set-Cookie: session=tok123"), std::string::npos);
  EXPECT_NE(raw.find("HttpOnly"), std::string::npos);
  EXPECT_NE(raw.find("Secure"), std::string::npos);
  EXPECT_NE(raw.find("Max-Age=3600"), std::string::npos);
  EXPECT_NE(raw.find("SameSite=Lax"), std::string::npos);
  EXPECT_NE(raw.find("Set-Cookie: pref=dark"), std::string::npos);
}

TEST(RouterTest, MethodNotAllowed) {
  Router router;
  router.add_route(Method::Get, "/items",
                   Handler([](const Request &, Response &) {}));

  // GET /items exists → path_exists returns true
  EXPECT_TRUE(router.path_exists("/items"));

  // DELETE /items doesn't exist → match returns monostate, but path_exists true
  std::unordered_map<std::string, std::string> params;
  auto handler = router.match(Method::Delete, "/items", params);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(handler));
  EXPECT_TRUE(router.path_exists("/items"));

  // Unknown path → path_exists false
  EXPECT_FALSE(router.path_exists("/nonexistent"));
}

TEST(RouterTest, BasicMatch) {
  Router router;
  bool called = false;
  router.add_route(Method::Get, "/hello",
                   Handler([&](const Request &, Response &) { called = true; }));

  std::unordered_map<std::string, std::string> params;
  auto handler = router.match(Method::Get, "/hello", params);
  ASSERT_TRUE(std::holds_alternative<Handler>(handler));

  Request req;
  Response res;
  std::get<Handler>(handler)(req, res);
  EXPECT_TRUE(called);
}

TEST(RouterTest, ParamMatch) {
  Router router;
  router.add_route(Method::Get, "/user/:id",
                   Handler([](const Request &, Response &) {}));

  std::unordered_map<std::string, std::string> params;
  auto handler = router.match(Method::Get, "/user/123", params);
  ASSERT_TRUE(std::holds_alternative<Handler>(handler));
  EXPECT_EQ(params["id"], "123");
}

TEST(RouterTest, NoMatch) {
  Router router;

  std::unordered_map<std::string, std::string> params;
  auto handler = router.match(Method::Get, "/nonexistent", params);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(handler));
}

// ─── RequestIdTest ────────────────────────────────────────────────────────────

TEST(RequestIdTest, EchosIncoming) {
  auto mw = qbuem::middleware::request_id();

  Request req;
  req.add_header("X-Request-ID", "existing-id-abc");
  Response res;
  bool cont = mw(req, res);

  EXPECT_TRUE(cont);
  // The existing ID is echoed back in the response
  std::string raw = res.serialize();
  EXPECT_NE(raw.find("X-Request-ID: existing-id-abc"), std::string::npos);
}

TEST(RequestIdTest, GeneratesNew) {
  auto mw = qbuem::middleware::request_id();

  Request req;  // no X-Request-ID header
  Response res;
  bool cont = mw(req, res);

  EXPECT_TRUE(cont);
  // A UUID v4 was generated (format: 8-4-4-4-12 hex chars)
  std::string raw = res.serialize();
  EXPECT_NE(raw.find("X-Request-ID: "), std::string::npos);
}

TEST(RequestIdTest, CustomHeaderName) {
  auto mw = qbuem::middleware::request_id("X-Trace-ID");

  Request req;
  req.add_header("X-Trace-ID", "trace-xyz");
  Response res;
  mw(req, res);

  EXPECT_NE(res.serialize().find("X-Trace-ID: trace-xyz"), std::string::npos);
}

// ─── RateLimitTest ────────────────────────────────────────────────────────────

TEST(RateLimitTest, AllowsNormal) {
  // burst=5: first 5 requests should all pass
  auto mw = qbuem::middleware::rate_limit({.rate_per_sec = 10.0, .burst = 5.0});

  for (int i = 0; i < 5; ++i) {
    Request req;
    req.add_header("X-Real-IP", "1.2.3.4");
    Response res;
    EXPECT_TRUE(mw(req, res)) << "request " << i << " should be allowed";
  }
}

TEST(RateLimitTest, BlocksExcess) {
  // burst=2: third request should be blocked
  auto mw = qbuem::middleware::rate_limit({.rate_per_sec = 1.0, .burst = 2.0});

  Request req;
  req.add_header("X-Real-IP", "9.9.9.9");

  Response r0, r1, r2;
  EXPECT_TRUE(mw(req, r0));
  EXPECT_TRUE(mw(req, r1));
  EXPECT_FALSE(mw(req, r2));  // bucket empty → 429

  std::string raw = r2.serialize();
  EXPECT_NE(raw.find("429"), std::string::npos);
  EXPECT_NE(raw.find("Retry-After"), std::string::npos);
}

TEST(RateLimitTest, SetsRateLimitHeaders) {
  auto mw = qbuem::middleware::rate_limit({.rate_per_sec = 10.0, .burst = 10.0});

  Request req;
  Response res;
  mw(req, res);

  std::string raw = res.serialize();
  EXPECT_NE(raw.find("X-RateLimit-Limit"), std::string::npos);
  EXPECT_NE(raw.find("X-RateLimit-Remaining"), std::string::npos);
}

// ─── SecurityHeadersTest ──────────────────────────────────────────────────────

TEST(SecurityHeadersTest, SecureHeadersBundle) {
  auto mw = qbuem::middleware::secure_headers();

  Request req;
  Response res;
  bool cont = mw(req, res);

  EXPECT_TRUE(cont);
  std::string raw = res.serialize();
  EXPECT_NE(raw.find("Strict-Transport-Security:"), std::string::npos);
  EXPECT_NE(raw.find("Content-Security-Policy:"),   std::string::npos);
  EXPECT_NE(raw.find("X-Frame-Options:"),            std::string::npos);
  EXPECT_NE(raw.find("X-Content-Type-Options: nosniff"), std::string::npos);
  EXPECT_NE(raw.find("Referrer-Policy:"),            std::string::npos);
}

TEST(SecurityHeadersTest, HstsValues) {
  auto mw = qbuem::middleware::hsts(3600, true, true);

  Request req;
  Response res;
  mw(req, res);

  std::string raw = res.serialize();
  EXPECT_NE(raw.find("max-age=3600"),      std::string::npos);
  EXPECT_NE(raw.find("includeSubDomains"), std::string::npos);
  EXPECT_NE(raw.find("preload"),           std::string::npos);
}

TEST(SecurityHeadersTest, IndividualHelpers) {
  {
    Request req; Response res;
    qbuem::middleware::csp("default-src 'none'")(req, res);
    EXPECT_NE(res.serialize().find("Content-Security-Policy: default-src 'none'"),
              std::string::npos);
  }
  {
    Request req; Response res;
    qbuem::middleware::x_frame_options("DENY")(req, res);
    EXPECT_NE(res.serialize().find("X-Frame-Options: DENY"), std::string::npos);
  }
  {
    Request req; Response res;
    qbuem::middleware::x_content_type_options()(req, res);
    EXPECT_NE(res.serialize().find("X-Content-Type-Options: nosniff"),
              std::string::npos);
  }
  {
    Request req; Response res;
    qbuem::middleware::referrer_policy("no-referrer")(req, res);
    EXPECT_NE(res.serialize().find("Referrer-Policy: no-referrer"),
              std::string::npos);
  }
  {
    Request req; Response res;
    qbuem::middleware::permissions_policy("camera=()")(req, res);
    EXPECT_NE(res.serialize().find("Permissions-Policy: camera=()"),
              std::string::npos);
  }
}

// ─── RangeRequestTest ─────────────────────────────────────────────────────────
// Range request logic lives in qbuem.cpp's finalize() lambda; we test the
// Response accessors it depends on (get_body, status_code) here.

TEST(RangeRequestTest, ResponseGetBody) {
  Response res;
  res.body("hello world");
  EXPECT_EQ(res.get_body(), "hello world");
}

TEST(RangeRequestTest, ResponseStatusCode) {
  Response res;
  res.status(206);
  EXPECT_EQ(res.status_code(), 206);
}

TEST(RangeRequestTest, ContentRangeHeader) {
  // Verify a 206 response with Content-Range serializes correctly.
  Response res;
  res.status(206)
     .header("Content-Range", "bytes 0-4/11")
     .header("Accept-Ranges", "bytes")
     .body("hello");
  std::string raw = res.serialize();
  EXPECT_NE(raw.find("206"), std::string::npos);
  EXPECT_NE(raw.find("Content-Range: bytes 0-4/11"), std::string::npos);
  EXPECT_EQ(res.get_body(), "hello");
}

TEST(RangeRequestTest, Status416) {
  Response res;
  res.status(416).header("Content-Range", "bytes */100").body("Range Not Satisfiable");
  std::string raw = res.serialize();
  EXPECT_NE(raw.find("416"), std::string::npos);
  EXPECT_NE(raw.find("bytes */100"), std::string::npos);
}

// ─── CryptoTest ───────────────────────────────────────────────────────────────

TEST(CryptoTest, ConstantTimeEqualSame) {
  EXPECT_TRUE(qbuem::constant_time_equal("hello", "hello"));
  EXPECT_TRUE(qbuem::constant_time_equal("", ""));
}

TEST(CryptoTest, ConstantTimeEqualDifferent) {
  EXPECT_FALSE(qbuem::constant_time_equal("hello", "world"));
  EXPECT_FALSE(qbuem::constant_time_equal("abc", "ab"));
  EXPECT_FALSE(qbuem::constant_time_equal("", "x"));
}

TEST(CryptoTest, RandomBytesLength) {
  auto r16 = qbuem::random_bytes(16);
  EXPECT_EQ(r16.size(), 16u);
  auto r32 = qbuem::random_bytes(32);
  EXPECT_EQ(r32.size(), 32u);
}

TEST(CryptoTest, RandomBytesUnique) {
  // Two 128-bit samples should not collide (astronomically unlikely)
  auto a = qbuem::random_bytes(16);
  auto b = qbuem::random_bytes(16);
  EXPECT_NE(a, b);
}

TEST(CryptoTest, CsrfTokenFormat) {
  auto tok = qbuem::csrf_token();
  // 128 bits = 16 bytes → ceil(16*4/3) = 22 Base64url chars
  EXPECT_GE(tok.size(), 21u);
  // All characters must be Base64url alphabet (A-Z a-z 0-9 - _)
  for (char c : tok) {
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_';
    EXPECT_TRUE(ok) << "unexpected char: " << c;
  }
}

// ─── ResponseEtagTest ─────────────────────────────────────────────────────────

TEST(ResponseEtagTest, EtagWrapsValue) {
  Response res;
  res.etag("abc123");
  std::string raw = res.serialize();
  EXPECT_NE(raw.find("ETag: \"abc123\""), std::string::npos);
}

TEST(ResponseEtagTest, EtagPreQuoted) {
  Response res;
  res.etag("\"already-quoted\"");
  std::string raw = res.serialize();
  // Should not double-wrap
  EXPECT_EQ(raw.find("ETag: \"\""), std::string::npos);
  EXPECT_NE(raw.find("ETag: \"already-quoted\""), std::string::npos);
}

TEST(ResponseEtagTest, GetHeaderAccessor) {
  Response res;
  res.header("X-Custom", "myval");
  EXPECT_EQ(res.get_header("X-Custom"), "myval");
  EXPECT_EQ(res.get_header("X-Missing"), "");
}

TEST(ResponseEtagTest, LastModifiedFormat) {
  Response res;
  std::time_t t = 0;  // 1970-01-01 00:00:00 UTC
  res.last_modified(t);
  std::string raw = res.serialize();
  EXPECT_NE(raw.find("Last-Modified: Thu, 01 Jan 1970 00:00:00 GMT"),
            std::string::npos);
}

// ─── StaticFilesTest ──────────────────────────────────────────────────────────

TEST(StaticFilesTest, MimeTypeDetection) {
  using qbuem::middleware::mime_type;
  EXPECT_EQ(mime_type(".html"),  "text/html; charset=utf-8");
  EXPECT_EQ(mime_type(".js"),    "text/javascript; charset=utf-8");
  EXPECT_EQ(mime_type(".css"),   "text/css; charset=utf-8");
  EXPECT_EQ(mime_type(".json"),  "application/json");
  EXPECT_EQ(mime_type(".png"),   "image/png");
  EXPECT_EQ(mime_type(".woff2"), "font/woff2");
  EXPECT_EQ(mime_type(".xyz"),   "application/octet-stream"); // unknown
}

TEST(StaticFilesTest, FileExtension) {
  using qbuem::middleware::file_extension;
  EXPECT_EQ(file_extension("/foo/bar.js"),  ".js");
  EXPECT_EQ(file_extension("style.css"),    ".css");
  EXPECT_EQ(file_extension("/noext"),       "");
  EXPECT_EQ(file_extension("/a.b/noext"),   "");
  EXPECT_EQ(file_extension("/a.b/c.d"),     ".d");
}

TEST(StaticFilesTest, ServeFileNotFound) {
  Response res;
  qbuem::middleware::serve_file("/nonexistent/path/file.txt", res);
  EXPECT_EQ(res.status_code(), 404);
}

TEST(StaticFilesTest, ServeFileExists) {
  // Write a temp file and serve it
  const char *tmp = "/tmp/qbuem_static_test.html";
  const std::string expected_content = "<h1>hello</h1>";
  {
    std::ofstream f(tmp);
    f << expected_content;
  }
  Response res;
  qbuem::middleware::serve_file(tmp, res);
  EXPECT_EQ(res.status_code(), 200);
  EXPECT_NE(res.serialize().find("text/html"), std::string::npos);
  EXPECT_NE(res.serialize().find("ETag"), std::string::npos);
  EXPECT_NE(res.serialize().find("Last-Modified"), std::string::npos);
#ifdef __linux__
  // On Linux, serve_file uses sendfile(2) — body is not loaded into memory.
  EXPECT_TRUE(res.has_sendfile());
  EXPECT_EQ(res.get_sendfile_path(), tmp);
  EXPECT_EQ(res.sendfile_size(), expected_content.size());
#else
  EXPECT_EQ(res.get_body(), expected_content);
#endif
  ::unlink(tmp);
}

// ─── RouterPrefixTest ─────────────────────────────────────────────────────────

TEST(RouterPrefixTest, PrefixRouteMatchesSuffix) {
  Router router;
  std::string captured_suffix;
  router.add_prefix_route(
      Method::Get, "/static",
      Handler([&](const Request &req, Response &) {
        captured_suffix = std::string(req.param("**"));
      }));

  std::unordered_map<std::string, std::string> params;
  auto h = router.match(Method::Get, "/static/js/app.js", params);
  ASSERT_TRUE(std::holds_alternative<Handler>(h));
  EXPECT_EQ(params["**"], "/js/app.js");
}

TEST(RouterPrefixTest, PrefixRoutePathExists) {
  Router router;
  router.add_prefix_route(Method::Get, "/assets",
                           Handler([](const Request &, Response &) {}));

  EXPECT_TRUE(router.path_exists("/assets/logo.png"));
  EXPECT_FALSE(router.path_exists("/other"));
}

TEST(RouterPrefixTest, ExactRouteBeforePrefix) {
  Router router;
  bool exact_called = false;
  router.add_route(Method::Get, "/static/special",
                   Handler([&](const Request &, Response &) {
                     exact_called = true;
                   }));
  router.add_prefix_route(Method::Get, "/static",
                           Handler([](const Request &, Response &) {}));

  std::unordered_map<std::string, std::string> params;
  auto h = router.match(Method::Get, "/static/special", params);
  ASSERT_TRUE(std::holds_alternative<Handler>(h));
  Request req; Response res;
  std::get<Handler>(h)(req, res);
  EXPECT_TRUE(exact_called); // exact match wins over prefix
}

// ─── MetricsTest ──────────────────────────────────────────────────────────────

TEST(MetricsTest, SnapshotInitialValues) {
  App app(1);
  auto m = app.snapshot_metrics();
  EXPECT_EQ(m.requests_total,     0u);
  EXPECT_EQ(m.errors_total,       0u);
  EXPECT_EQ(m.active_connections, 0u);
  EXPECT_EQ(m.bytes_sent,         0u);
}

TEST(MetricsTest, AtomicCountersWritable) {
  App app(1);
  app.cnt_requests_.store(42, std::memory_order_relaxed);
  app.cnt_errors_.store(3, std::memory_order_relaxed);
  auto m = app.snapshot_metrics();
  EXPECT_EQ(m.requests_total, 42u);
  EXPECT_EQ(m.errors_total,   3u);
}

// ─── AccessLoggerTest ─────────────────────────────────────────────────────────

TEST(AccessLoggerTest, SetLoggerCalled) {
  // Access logger is a callback set via App::set_access_logger().
  // We verify the App API compiles and accepts a lambda.
  App app(1);
  bool called = false;
  app.set_access_logger([&](std::string_view, std::string_view, int, long) {
    called = true;
  });
  // Logger is called inside listen() when requests are handled; we just verify
  // the API is wired up by checking the App accepted the callback (no crash).
  EXPECT_FALSE(called); // Not called until a request is processed
}

// ─── UrlTest ──────────────────────────────────────────────────────────────────

#include <qbuem/url.hpp>

TEST(UrlTest, DecodePercent) {
  EXPECT_EQ(qbuem::url_decode("Hello%20World"), "Hello World");
  EXPECT_EQ(qbuem::url_decode("%2F"),           "/");
  EXPECT_EQ(qbuem::url_decode("%2f"),           "/");
  EXPECT_EQ(qbuem::url_decode("no+encoding"),   "no encoding"); // '+' → space
}

TEST(UrlTest, DecodeEmpty) {
  EXPECT_EQ(qbuem::url_decode(""), "");
}

TEST(UrlTest, DecodeUnreserved) {
  EXPECT_EQ(qbuem::url_decode("abcABC-_.~"), "abcABC-_.~");
}

TEST(UrlTest, EncodeSafeChars) {
  EXPECT_EQ(qbuem::url_encode("abcABC-_.~"), "abcABC-_.~");
}

TEST(UrlTest, EncodeSpecialChars) {
  EXPECT_EQ(qbuem::url_encode(" "),     "%20");
  EXPECT_EQ(qbuem::url_encode("/"),     "%2F");
  EXPECT_EQ(qbuem::url_encode("a b/c"), "a%20b%2Fc");
}

TEST(UrlTest, RoundTrip) {
  std::string original = "hello world / test=1&foo=bar";
  EXPECT_EQ(qbuem::url_decode(qbuem::url_encode(original)), original);
}

// ─── RemoteAddrTest ───────────────────────────────────────────────────────────

TEST(RemoteAddrTest, DefaultEmpty) {
  Request req;
  EXPECT_EQ(req.remote_addr(), "");
}

TEST(RemoteAddrTest, SetAndGet) {
  Request req;
  req.set_remote_addr("192.168.1.1");
  EXPECT_EQ(req.remote_addr(), "192.168.1.1");
}

TEST(RemoteAddrTest, IPv6Address) {
  Request req;
  req.set_remote_addr("::1");
  EXPECT_EQ(req.remote_addr(), "::1");
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
