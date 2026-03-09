#include <draco/draco.hpp>
#include <iostream>

int main() {
  draco::App app;

  // Logging middleware
  app.use([](const draco::Request &req, draco::Response &) {
    std::cout << "[LOG] " << (int)req.method() << " " << req.path()
              << std::endl;
    return true;
  });

  app.get("/", [](const draco::Request &, draco::Response &res) {
    res.status(200).body("Hello from Draco WAS Phase 2!");
  });

  app.get("/user/:id", [](const draco::Request &req, draco::Response &res) {
    (void)req;
    // In a real app, we'd extract params. For now, just a proof of concept.
    res.status(200).body("User profile requested");
  });

  auto res = app.listen(8080);
  if (!res) {
    std::cerr << "Failed to start server: " << res.error().message()
              << std::endl;
    return 1;
  }

  return 0;
}