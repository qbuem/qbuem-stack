/**
 * coro_json.cpp — qbuem-stack JSON example
 *
 * JSON library: qbuem-json (https://github.com/qbuem/qbuem-json)
 *   Same API as the former beast-json; only the namespace changed to qbuem::.
 *
 * The qbuem-stack framework core does not depend on any specific JSON library.
 * req.body() is delivered as raw bytes (std::string_view); the application
 * parses it directly.
 *
 * Build:
 *   cmake -DQBUEM_BUILD_EXAMPLES=ON ..
 *   make coro_json
 *
 * Test:
 *   curl http://localhost:8080/user/42
 *   curl -X POST http://localhost:8080/echo \
 *        -H "Content-Type: application/json" \
 *        -d '{"message":"hello","name":"qbuem"}'
 */
#include <qbuem_json/qbuem_json.hpp>
#include <qbuem/core/awaiters.hpp>
#include <qbuem/qbuem_stack.hpp>
#include <qbuem/compat/print.hpp>

#include <string>

using namespace qbuem;

// ─── Response DTOs ───────────────────────────────────────────────────────────
struct UserResponse {
    std::string message;
    std::string status;
    std::string version;
    std::string user_id;
};
QBUEM_JSON_FIELDS(UserResponse, message, status, version, user_id)

struct EchoResponse {
    std::string echo_message;
    std::string echo_name;
    bool        ok = true;
};
QBUEM_JSON_FIELDS(EchoResponse, echo_message, echo_name, ok)

// ─── JSON parsing helper (DOM engine) ────────────────────────────────────────
// req.body() is raw bytes (std::string_view).
// Uses the DOM engine to demonstrate SafeValue chaining.
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

// GET /user/:id — async coroutine handler
Task<void> async_user_handler(const Request& req, Response& res) {
    std::println("[Async] GET {}", req.path());

    co_await sleep(0); // yield control to the reactor once (coroutine demo)

    res.status(200)
       .header("Content-Type", "application/json")
       .body(qbuem::write(UserResponse{
           "Hello from qbuem-stack Async Coroutines!",
           "success",
           std::string(Version::string),
           std::string(req.param("id")),
       }));
    co_return;
}

// POST /echo — parse request with DOM SafeValue chaining + serialize response with Nexus
Task<void> echo_handler(const Request& req, Response& res) {
    qbuem::Value body = parse_body(req);

    // .get("key") | fallback — propagates nullopt when key is missing or type mismatch
    std::string msg  = body.get("message") | std::string("(no message)");
    std::string name = body.get("name")    | std::string("(anonymous)");

    res.status(200)
       .header("Content-Type", "application/json")
       .body(qbuem::write(EchoResponse{msg, name, true}));
    co_return;
}

int main() {
    App app;

    app.get("/user/:id", AsyncHandler(async_user_handler));
    app.post("/echo",    AsyncHandler(echo_handler));

    app.get("/", Handler([](const Request&, Response& res) {
        res.status(200).body("Welcome to qbuem-stack!");
    }));

    std::println("coro_json example: http://0.0.0.0:8080");
    std::println("  GET  /user/42");
    std::println("  POST /echo  body: {{\"message\":\"hi\",\"name\":\"qbuem\"}}");

    if (auto r = app.listen(8080); !r) {
        std::println(stderr, "listen failed: {}", r.error().message());
        return 1;
    }
}
