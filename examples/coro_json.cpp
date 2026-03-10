/**
 * coro_json.cpp — Draco WAS JSON example
 *
 * JSON 라이브러리: beast_json (https://github.com/the-lkb/beast-json)
 *
 * Draco 프레임워크는 특정 JSON 라이브러리에 의존하지 않습니다.
 * 모든 예제는 beast_json을 사용하며, 발견된 버그는 아래 가이드에 따라 리포트합니다:
 *   → CONTRIBUTING.md § "beast_json 버그 리포팅"
 */
#include <beast_json/beast_json.hpp>
#include <draco/core/awaiters.hpp>
#include <draco/draco.hpp>
#include <iostream>
#include <string>

using namespace draco;

// ─── JSON 파싱 헬퍼 ──────────────────────────────────────────────────────────
// req.body() 는 raw bytes(std::string_view) 입니다.
// 애플리케이션이 원하는 JSON 라이브러리로 직접 파싱합니다.
static beast::Value parse_body(const Request &req) {
  beast::Document doc;
  std::string_view b = req.body();
  if (b.empty())
    return beast::Value{};
  try {
    return beast::parse(doc, b);
  } catch (...) {
    return beast::Value{};
  }
}

// GET /user/:id — 비동기 핸들러
Task<void> async_handler(const Request &req, Response &res) {
  std::cout << "[Async] GET " << req.path() << std::endl;

  co_await sleep(0); // reactor에 제어권을 한 번 돌려줌 (coroutine 시연)

  beast::Document doc;
  auto out = beast::parse(doc, "{}");
  out.insert("message", "Hello from Draco Async Coroutines!");
  out.insert("status", "success");
  out.insert("version", Version::string);
  out.insert("user_id", std::string(req.param("id")));

  res.status(200)
      .header("Content-Type", "application/json")
      .body(out.dump());
  co_return;
}

// POST /echo — beast_json SafeValue 체인 시연
Task<void> echo_handler(const Request &req, Response &res) {
  beast::Value body = parse_body(req);

  // .get("key") | fallback  — 키 없거나 타입 불일치 시 nullopt 전파
  std::string msg  = body.get("message") | std::string("(no message)");
  std::string name = body.get("name")    | std::string("(anonymous)");

  beast::Document doc;
  auto out = beast::parse(doc, "{}");
  out.insert("echo_message", msg);
  out.insert("echo_name", name);
  out.insert("ok", true);

  res.status(200)
      .header("Content-Type", "application/json")
      .body(out.dump());
  co_return;
}

int main() {
  App app;

  app.get("/user/:id", AsyncHandler(async_handler));
  app.post("/echo",    AsyncHandler(echo_handler));

  app.get("/", Handler([](const Request &, Response &res) {
    res.status(200).body("Welcome to Draco WAS!");
  }));

  std::cout << "coro_json example: http://0.0.0.0:8080\n"
            << "  GET  /user/42\n"
            << "  POST /echo  body: {\"message\":\"hi\",\"name\":\"draco\"}\n";

  if (auto r = app.listen(8080); !r) {
    std::cerr << "listen failed: " << r.error().message() << "\n";
    return 1;
  }
}
