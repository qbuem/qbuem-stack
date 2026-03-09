# 🐉 Draco WAS

**Draco WAS** is an ultra-low latency, C++23 Web Application Server framework designed for modern REST APIs. It combines the extreme performance of systems programming with the developer-friendly ergonomics of frameworks like Express.js or FastAPI.

## ✨ Why Draco WAS?

Most C++ web frameworks are notoriously difficult to configure, overly verbose, or sacrifice performance for abstractions. Draco WAS solves this by leveraging C++23 features to deliver:

1. **Extreme Usability**: Spin up a multi-threaded, non-blocking REST API server in under 10 lines of code.
2. **Zero-Overhead Abstractions**: Compile-time routing and thread-local memory arenas guarantee consistent microsecond latencies.
3. **Native Beast JSON**: Seamlessly integrated with Beast JSON, the fastest C++20 JSON library, for zero-copy parsing and high-throughput serialization.
4. **Modern C++23**: Built with coroutines (`co_await`), `std::expected`, and compile-time string processing.

## 🚀 Quick Start (Conceptual)

Draco aims for zero boilerplate. Here is what an application looks like:

```cpp
#include <draco/draco.hpp>
#include <beast_json/beast_json.hpp>

struct User {
    uint64_t id;
    std::string name;
};
BEAST_JSON_FIELDS(User, id, name)

int main() {
    draco::App app;

    // Simple GET returning JSON
    app.get("/api/ping", [](const draco::Request& req, draco::Response& res) {
        res.json(beast::json::Value{{"status", "ok"}});
    });

    // Path parameters and auto-serialization
    app.get("/api/users/:id", [](auto req, auto res) {
        uint64_t id = req.path_param<uint64_t>("id");
        User user{id, "Alice"};
        res.json(user); // Auto-serialized by Beast JSON
    });

    // Modern Coroutine Support
    app.post("/api/data", [](auto req, auto res) -> draco::Task<void> {
        auto json = req.json(); // Parses request body instantly
        co_await db::save(json);
        res.status(201).send("Created");
    });

    app.listen(8080);
    return 0;
}
```

## 🛠 Features

### Standard WAS Framework Features
* **HTTP/1.1 & HTTP/2** support.
* **Middleware Pipeline**: Easy integration of CORS, Authentication, Logging, and Rate Limiting.
* **Multipart/Form-Data**: Native handling for file uploads.
* **TLS/SSL**: Built-in support for secure connections.
* **Connection Pooling**: Native tools for managing upstream database or Redis connections.
* **WebSockets & SSE**: Real-time bidirectional streaming support.

### Draco Exclusive Features 🌟
* **Compile-Time Radix Tree Router**: Route matching happens in nanoseconds with zero mallocs.
* **Shared-Nothing Multi-Threading**: Lock-free, thread-per-core event loops maximizing hardware utilization and preventing CPU cache trashing.
* **Auto-OpenAPI Generation**: Generate Swagger UI definitions directly from your route handlers.
* **Arena Allocation**: Memory is allocated via thread-local arenas per HTTP request lifecycle, meaning `new`/`delete` are effectively eliminated from the hot path.

## 🔧 Build & Installation

*(Coming soon! Draco requires a C++23 compiler like GCC 13+ or Clang 16+).*

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j
```

---
*Created by The LKB Innovations.*
