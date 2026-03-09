#include <beast_json/beast_json.hpp>
#include <draco/draco.hpp>
#include <iostream>

using namespace draco;

Task<void> async_handler(const Request &req, Response &res) {
  std::cout << "[Async] Handling request for: " << req.path() << std::endl;

  // Simulate some async work (in Phase 3 this is still synchronous execution
  // but using Coroutine interface) In Phase 4, we'll add real async awaiters
  // for I/O and timers.

  beast::Document doc;
  auto response_data = beast::parse(doc, "{}");
  response_data.insert("message", "Hello from Draco Async Coroutines!");
  response_data.insert("status", "success");
  response_data.insert("version", Version::string);

  // Demonstrate parameter extraction
  response_data.insert("user_id", std::string(req.param("id")));

  res.status(200).json(response_data);
  co_return;
}

int main() {
  App app;

  // Async handler registration
  app.get("/user/:id", AsyncHandler(async_handler));

  // Sync handler registration (for comparison)
  app.get("/", Handler([](const Request &, Response &res) {
            res.status(200).body("Welcome to Draco WAS Phase 3!");
          }));

  if (auto res = app.listen(8080); !res) {
    std::cerr << "Failed to listen: " << res.error().message() << std::endl;
    return 1;
  }

  return 0;
}
