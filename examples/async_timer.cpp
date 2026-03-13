#include <qbuem/core/awaiters.hpp>
#include <qbuem/qbuem-stack.hpp>
#include <iostream>

using namespace qbuem;

int main() {
  App app;

  app.get("/hello", Handler([](const Request &req, Response &res) {
            res.status(200).body("Hello from sync handler!");
          }));

  app.get("/sleep",
          AsyncHandler([](const Request &req, Response &res) -> Task<void> {
            std::cout << "[Server] Handling /sleep request..." << std::endl;
            co_await sleep(1000); // 1-second non-blocking delay
            std::cout << "[Server] Resuming after 1s sleep" << std::endl;
            res.status(200).body("Hello after 1s sleep!");
            co_return;
          }));

  std::cout << "Draco WAS Async Timer Example running on http://0.0.0.0:8080"
            << std::endl;
  std::cout << "Try: curl http://localhost:8080/sleep" << std::endl;

  if (auto result = app.listen(8080); !result) {
    std::cerr << "Failed to listen: " << result.error().message() << std::endl;
    return 1;
  }
}
