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

TEST(RouterTest, BasicMatch) {
  Router router;
  bool called = false;
  router.add_route(Method::Get, "/hello",
                   [&](const Request &, Response &) { called = true; });

  std::unordered_map<std::string, std::string> params;
  auto handler = router.match(Method::Get, "/hello", params);
  ASSERT_NE(handler, nullptr);

  Request req;
  Response res;
  handler(req, res);
  EXPECT_TRUE(called);
}

TEST(RouterTest, ParamMatch) {
  Router router;
  router.add_route(Method::Get, "/user/:id",
                   [&](const Request &, Response &) {});

  std::unordered_map<std::string, std::string> params;
  auto handler = router.match(Method::Get, "/user/123", params);
  ASSERT_NE(handler, nullptr);
  EXPECT_EQ(params["id"], "123");
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
