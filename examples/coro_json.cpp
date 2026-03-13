/**
 * coro_json.cpp — qbuem-stack JSON 예제
 *
 * JSON 라이브러리: qbuem-json (https://github.com/qbuem/qbuem-json)
 *   구 beast-json과 동일한 API, 네임스페이스만 qbuem:: 으로 변경됨.
 *
 * qbuem-stack 프레임워크 코어는 특정 JSON 라이브러리에 의존하지 않습니다.
 * req.body() 는 raw bytes(std::string_view) 로 전달되며, 앱이 직접 파싱합니다.
 *
 * 빌드:
 *   cmake -DQBUEM_BUILD_EXAMPLES=ON ..
 *   make coro_json
 *
 * 테스트:
 *   curl http://localhost:8080/user/42
 *   curl -X POST http://localhost:8080/echo \
 *        -H "Content-Type: application/json" \
 *        -d '{"message":"hello","name":"qbuem"}'
 */
#include <qbuem_json/qbuem_json.hpp>
#include <qbuem/core/awaiters.hpp>
#include <qbuem/qbuem.hpp>
#include <iostream>
#include <string>

using namespace qbuem;

// ─── JSON 파싱 헬퍼 ──────────────────────────────────────────────────────────
// req.body() 는 raw bytes(std::string_view) 입니다.
// 애플리케이션이 원하는 JSON 라이브러리로 직접 파싱합니다.
static qbuem::Value parse_body(const Request& req) {
    qbuem::Document doc;
    std::string_view b = req.body();
    if (b.empty())
        return qbuem::Value{};
    try {
        return qbuem::parse(doc, b);
    } catch (...) {
        return qbuem::Value{};
    }
}

// GET /user/:id — 비동기 코루틴 핸들러
Task<void> async_user_handler(const Request& req, Response& res) {
    std::cout << "[Async] GET " << req.path() << std::endl;

    co_await sleep(0); // reactor 에 제어권을 한 번 양보 (coroutine 시연)

    qbuem::Document doc;
    auto out = qbuem::parse(doc, "{}");
    out.insert("message", "Hello from qbuem-stack Async Coroutines!");
    out.insert("status",  "success");
    out.insert("version", Version::string);
    out.insert("user_id", std::string(req.param("id")));

    res.status(200)
       .header("Content-Type", "application/json")
       .body(out.dump());
    co_return;
}

// POST /echo — qbuem-json SafeValue 체인 시연
Task<void> echo_handler(const Request& req, Response& res) {
    qbuem::Value body = parse_body(req);

    // .get("key") | fallback — 키 없거나 타입 불일치 시 nullopt 전파
    std::string msg  = body.get("message") | std::string("(no message)");
    std::string name = body.get("name")    | std::string("(anonymous)");

    qbuem::Document out_doc;
    auto out = qbuem::parse(out_doc, "{}");
    out.insert("echo_message", msg);
    out.insert("echo_name",    name);
    out.insert("ok", true);

    res.status(200)
       .header("Content-Type", "application/json")
       .body(out.dump());
    co_return;
}

int main() {
    App app;

    app.get("/user/:id", AsyncHandler(async_user_handler));
    app.post("/echo",    AsyncHandler(echo_handler));

    app.get("/", Handler([](const Request&, Response& res) {
        res.status(200).body("Welcome to qbuem-stack!");
    }));

    std::cout << "coro_json example: http://0.0.0.0:8080\n"
              << "  GET  /user/42\n"
              << "  POST /echo  body: {\"message\":\"hi\",\"name\":\"qbuem\"}\n";

    if (auto r = app.listen(8080); !r) {
        std::cerr << "listen failed: " << r.error().message() << "\n";
        return 1;
    }
}
