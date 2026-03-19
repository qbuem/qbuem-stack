#include <qbuem/qbuem_stack.hpp>
#include <qbuem/compat/print.hpp>

int main() {
  qbuem::App app;

  // Logging middleware
  app.use([](const qbuem::Request &req, qbuem::Response &) {
    std::println("[LOG] {} {}", (int)req.method(), req.path());
    return true;
  });

  app.get("/", [](const qbuem::Request &, qbuem::Response &res) {
    res.status(200).body("Hello from qbuem-stack!");
  });

  app.get("/user/:id", [](const qbuem::Request &req, qbuem::Response &res) {
    (void)req;
    // In a real app, we'd extract params. For now, just a proof of concept.
    res.status(200).body("User profile requested");
  });

  auto res = app.listen(8080);
  if (!res) {
    std::println(stderr, "Failed to start server: {}", res.error().message());
    return 1;
  }

  return 0;
}
