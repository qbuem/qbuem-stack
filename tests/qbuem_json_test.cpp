/**
 * @file tests/qbuem_json_test.cpp
 * @brief Integration tests: qbuem-json + qbuem-stack HTTP layer.
 *
 * Verifies that qbuem-json can be used seamlessly with qbuem-stack
 * Request/Response types for JSON body parsing and serialization.
 *
 * These tests also serve as qbuem-json correctness checks — if qbuem-json
 * misbehaves (parse error, wrong value type, missing key handling, etc.)
 * the failure shows up here rather than in production.
 */

#include <gtest/gtest.h>

#ifdef QBUEM_HAS_QBUEM_JSON

#include <qbuem_json/qbuem_json.hpp>

#include <draco/http/parser.hpp>
#include <draco/http/request.hpp>
#include <draco/http/response.hpp>

// ─── Helper: build a minimal POST Request with a JSON body ───────────────────

static draco::Request make_post_request(std::string_view body) {
  std::string raw =
      "POST /echo HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " + std::to_string(body.size()) + "\r\n"
      "\r\n" + std::string(body);

  draco::HttpParser parser;
  draco::Request req;
  parser.parse(raw, req);
  return req;
}

// ─── Parse ───────────────────────────────────────────────────────────────────

TEST(QbuemJsonTest, ParseValidObject) {
  auto req = make_post_request(R"({"name":"alice","age":30})");
  ASSERT_FALSE(req.body().empty());

  qbuem::Document doc;
  auto obj = qbuem::parse(doc, req.body());

  std::string name = obj.get("name") | std::string{};
  EXPECT_EQ(name, "alice");

  int age = obj.get("age") | 0;
  EXPECT_EQ(age, 30);
}

TEST(QbuemJsonTest, ParseMissingKeyFallback) {
  auto req = make_post_request(R"({"x":1})");
  qbuem::Document doc;
  auto obj = qbuem::parse(doc, req.body());

  // Missing key: SafeValue propagates nullopt → fallback via operator|
  std::string missing = obj.get("not_there") | std::string("default");
  EXPECT_EQ(missing, "default");
}

TEST(QbuemJsonTest, ParseNestedObject) {
  auto req = make_post_request(R"({"user":{"id":42,"role":"admin"}})");
  qbuem::Document doc;
  auto obj  = qbuem::parse(doc, req.body());
  // Navigate nested value via SafeValue::get() monadic chain
  int id           = obj.get("user").get("id")   | 0;
  std::string role = obj.get("user").get("role") | std::string{};

  EXPECT_EQ(id,   42);
  EXPECT_EQ(role, "admin");
}

TEST(QbuemJsonTest, ParseArray) {
  auto req = make_post_request(R"({"tags":["go","cpp","rust"]})");
  qbuem::Document doc;
  auto obj  = qbuem::parse(doc, req.body());

  // Access array via Value::size() and Value::get(idx)
  auto tags_safe = obj.get("tags");
  ASSERT_TRUE(tags_safe);                      // key must exist
  EXPECT_EQ(tags_safe->size(), 3u);

  std::string first = tags_safe->get(0) | std::string{};
  std::string last  = tags_safe->get(2) | std::string{};
  EXPECT_EQ(first, "go");
  EXPECT_EQ(last,  "rust");
}

TEST(QbuemJsonTest, ParseBooleans) {
  auto req = make_post_request(R"({"ok":true,"fail":false})");
  qbuem::Document doc;
  auto obj = qbuem::parse(doc, req.body());

  bool ok   = obj.get("ok")   | false;
  bool fail = obj.get("fail") | true;
  EXPECT_TRUE(ok);
  EXPECT_FALSE(fail);
}

TEST(QbuemJsonTest, ParseNull) {
  auto req = make_post_request(R"({"data":null})");
  qbuem::Document doc;
  auto obj = qbuem::parse(doc, req.body());

  auto data = obj.get("data");
  ASSERT_TRUE(data);
  EXPECT_TRUE(data->is_null());
}

// ─── Serialize ───────────────────────────────────────────────────────────────

TEST(QbuemJsonTest, SerializeToResponse) {
  qbuem::Document doc;
  auto out = qbuem::parse(doc, "{}");
  out.insert("status", "ok");
  out.insert("code",   200);

  draco::Response res;
  res.status(200)
     .header("Content-Type", "application/json")
     .body(out.dump());

  EXPECT_EQ(res.status_code(), 200);
  EXPECT_FALSE(res.get_body().empty());
  EXPECT_NE(res.get_body().find("\"status\""), std::string::npos);
  EXPECT_NE(res.get_body().find("\"ok\""),     std::string::npos);
}

TEST(QbuemJsonTest, RoundTrip) {
  // Parse request body → transform → serialize back as response body.
  auto req = make_post_request(R"({"msg":"hello","n":7})");

  qbuem::Document in_doc;
  auto in_obj = qbuem::parse(in_doc, req.body());

  std::string msg = in_obj.get("msg") | std::string{};
  int n           = in_obj.get("n")   | 0;

  qbuem::Document out_doc;
  auto out = qbuem::parse(out_doc, "{}");
  out.insert("echo",      msg + "!");
  out.insert("n_plus_one", n + 1);

  draco::Response res;
  res.status(200)
     .header("Content-Type", "application/json")
     .body(out.dump());

  // Re-parse response body to verify round-trip correctness.
  qbuem::Document check_doc;
  auto check = qbuem::parse(check_doc, res.get_body());

  std::string echo = check.get("echo")       | std::string{};
  int n2           = check.get("n_plus_one") | 0;

  EXPECT_EQ(echo, "hello!");
  EXPECT_EQ(n2,   8);
}

TEST(QbuemJsonTest, TypeMismatchFallback) {
  // String accessed as int → SafeValue must return fallback, not crash.
  auto req = make_post_request(R"({"val":"not_a_number"})");
  qbuem::Document doc;
  auto obj = qbuem::parse(doc, req.body());

  int v = obj.get("val") | -1;
  EXPECT_EQ(v, -1);
}

TEST(QbuemJsonTest, EmptyObject) {
  auto req = make_post_request("{}");
  qbuem::Document doc;
  auto obj = qbuem::parse(doc, req.body());

  std::string x = obj.get("x") | std::string("fallback");
  EXPECT_EQ(x, "fallback");
}

TEST(QbuemJsonTest, DeepChain) {
  // Chained get() on nested keys via SafeValue::get() (monadic chain).
  auto req = make_post_request(R"({"a":{"b":{"c":99}}})");
  qbuem::Document doc;
  auto obj = qbuem::parse(doc, req.body());

  // Present chain — SafeValue::get() propagates the value
  int c = obj.get("a").get("b").get("c") | -1;
  EXPECT_EQ(c, 99);

  // Absent chain — SafeValue propagates nullopt silently, no throw/crash
  int missing = obj.get("a").get("z").get("c") | -99;
  EXPECT_EQ(missing, -99);
}

#else  // QBUEM_HAS_QBUEM_JSON not defined

// Placeholder so the test binary links even without qbuem-json.
TEST(QbuemJsonTest, Skipped) {
  GTEST_SKIP() << "qbuem-json not available at build time.";
}

#endif  // QBUEM_HAS_QBUEM_JSON
