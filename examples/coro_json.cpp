#include <beast_json/beast_json.hpp>
#include <draco/draco.hpp>
#include <iostream>

using namespace draco;

Task<void> async_handler(const Request &req, Response &res) {
  std::cout << "[Async] Handling request for: " << req.path() << std::endl;

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

// POST /echo — demonstrates safe JSON body access using beast::SafeValue.
// req.json() is now safe even for empty or malformed bodies (returns invalid
// Value{}). .get() propagates nullopt through missing keys without throwing.
Task<void> echo_handler(const Request &req, Response &res) {
  beast::Value body = req.json();

  std::string msg = body.get("message") | std::string("(no message)");
  std::string name = body.get("name") | std::string("(anonymous)");

  beast::Document doc;
  auto out = beast::parse(doc, "{}");
  out.insert("echo_message", msg);
  out.insert("echo_name", name);
  out.insert("ok", true);

  res.status(200).json(out);
  co_return;
}

int main() {
  App app;

  // Async handler registration
  app.get("/user/:id", AsyncHandler(async_handler));

  // POST /echo — safe JSON body parsing demo (beast_json SafeValue)
  app.post("/echo", AsyncHandler(echo_handler));

  // Sync handler registration (for comparison)
  app.get("/", Handler([](const draco::Request &, draco::Response &res) {
            res.status(200).body("Welcome to Draco WAS Phase 3!");
          }));

  if (auto res = app.listen(8080); !res) {
    std::cerr << "Failed to listen: " << res.error().message() << std::endl;
    return 1;
  }

  return 0;
}
