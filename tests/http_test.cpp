#include <draco/http/parser.hpp>
#include <draco/http/router.hpp>
#include <gtest/gtest.h>

using namespace draco;

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

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
