#include <qbuem/core/awaiters.hpp>
#include <qbuem/qbuem_stack.hpp>
#include <print>

using namespace qbuem;

int main() {
  App app;

  app.get("/hello", Handler([](const Request &req, Response &res) {
            res.status(200).body("Hello from sync handler!");
          }));

  app.get("/sleep",
          AsyncHandler([](const Request &req, Response &res) -> Task<void> {
            std::println("[Server] Handling /sleep request...");
            co_await sleep(1000); // 1-second non-blocking delay
            std::println("[Server] Resuming after 1s sleep");
            res.status(200).body("Hello after 1s sleep!");
            co_return;
          }));

  std::println("Draco WAS Async Timer Example running on http://0.0.0.0:8080");
  std::println("Try: curl http://localhost:8080/sleep");

  if (auto result = app.listen(8080); !result) {
    std::println(stderr, "Failed to listen: {}", result.error().message());
    return 1;
  }
}
