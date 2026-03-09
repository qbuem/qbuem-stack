# Draco WAS: Ultra-Low Latency C++23 Web Framework

## Development Roadmap & TODO

Draco WAS is designed to be the fastest, most ergonomic C++23 Web Application Server for REST APIs. It combines the raw performance of zero-copy infrastructure with the usability of modern web frameworks (like Express.js or FastAPI).

### Phase 1: Core Networking & HTTP/1.1 Engine (Foundation)
- [ ] Implement C++23 native epoll/kqueue event loop or integrate high-performance I/O backend (e.g., io_uring for Linux).
- [ ] Lock-free, thread-per-core (Shared-Nothing) architecture.
- [ ] Zero-allocation HTTP/1.1 parser (custom or heavily optimized llhttp wrapper).
- [ ] Thread-local memory arenas per request lifecycle to completely eliminate `new`/`malloc` overhead during request processing.
- [ ] Basic Request and Response object abstractions.

### Phase 2: Ultra-Fast Routing & REST Ergonomics
- [ ] **Compile-Time Routing**: Utilize C++23 `constexpr` and template metaprogramming to build a trie or radix tree at compile time (zero runtime route registration cost).
- [ ] Support for Path Parameters (e.g., `/api/users/:id`).
- [ ] Support for Query Parameters and form-data parsing.
- [ ] **Extreme Usability API**: Chainable, highly intuitive routing syntax (similar to Express/FastAPI) using C++20/23 lambdas.
- [ ] Automatic Request/Response JSON serialization via native **Beast JSON** integration.

### Phase 3: Modern Concurrency & Async I/O
- [ ] C++20/23 Coroutine (`co_await`) support for non-blocking database and external API calls.
- [ ] (Optional/Future) P2300 Senders/Receivers execution model integration.
- [ ] Connection pooling for upstream services (Database, Redis, etc.).

### Phase 4: Middleware, Security & Web Protocols
- [ ] Middleware pipeline support for pre/post-processing (Logging, Auth, CORS, Rate Limiting).
- [ ] TLS/SSL termination support (OpenSSL/BoringSSL).
- [ ] HTTP/2 (and potentially HTTP/3 QUIC) support.
- [ ] WebSocket and Server-Sent Events (SSE) support for real-time streaming.
- [ ] JWT authentication utilities.

### Phase 5: Draco Exclusive Features (The "Unfair Advantage")
- [ ] **Native Beast JSON Integration**: Direct-to-tape parse from socket buffer; zero-copy JSON DOM routing.
- [ ] **Auto-OpenAPI Generation**: Use C++23 reflection or macro attributes to automatically generate Swagger/OpenAPI 3.0 specs from the C++ route definitions.
- [ ] **Zero-Config Deployment**: Single binary static compilation, self-contained. YAML/JSON based simple server configuration.
- [ ] **Adaptive Backpressure**: Built-in OS-level backpressure handling to survive DDoS or extreme burst traffic without OOM (Out Of Memory).

### Phase 6: Developer Experience & Ecosystem
- [ ] Comprehensive Doxygen & VitePress documentation (Ah, that's easy! concept).
- [ ] Hot-reload development mode for fast C++ rebuilds.
- [ ] Pre-built ORM integration examples (e.g., sqlpp11 or similar).

---

## Architecture Brainstorming Notes
* **Event Loop**: Epoll (Linux), kqueue (macOS), io_uring (Linux modern).
* **Routing**: Since it's C++23, we should push string matching to compile-time wherever possible.
* **Ergonomics over everything**: If the user has to write more than 5 lines of code to spin up a server and handle a GET request returning JSON, we failed.
