#include <qbuem/qbuem.hpp>
#include <iostream>

int main() {
  qbuem::App app;

  // Logging middleware
  app.use([](const qbuem::Request &req, qbuem::Response &) {
    std::cout << "[LOG] " << (int)req.method() << " " << req.path()
              << std::endl;
    return true;
  });

  app.get("/", [](const qbuem::Request &, qbuem::Response &res) {
    res.status(200).body("Hello from Draco WAS Phase 2!");
  });

  app.get("/user/:id", [](const qbuem::Request &req, qbuem::Response &res) {
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