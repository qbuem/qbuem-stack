# Draco WAS Development Roadmap (v0.2.0)

Draco WAS is a C++20/23 ultra-low latency Web Application Server designed for extreme performance and zero-dependency agility.

## ✅ Phase 0-2 & L: Foundation & Core Engine (COMPLETED)
- [x] Expert architecture design (Shared-Nothing, io_uring, Coroutines).
- [x] C++23 project foundation & directory structure.
- [x] Dependency management via `FetchContent` (Beast-JSON, GTest).
- [x] **Reactor Core**: `kqueue` (macOS) and `io_uring` (Linux placeholder) implemented.
- [x] **Dispatcher**: Thread-per-core Dispatcher with CPU affinity support.
- [x] **HTTP Abstractions**: Zero-copy HTTP Parser, `Request`/`Response` API.
- [x] **Modern Routing**: Radix Tree for fast route matching.
- [x] **Library Infrastructure**: Refactored to header + source distribution with modern CMake.

## ✅ Phase 3: Coroutines & Beast JSON (COMPLETED)
- [x] **Async Flow**: Custom `draco::Task<T>` with symmetric transfer for nested coroutines.
- [x] **Beast JSON Integration**: Native integration in `Request`/`Response`.
- [x] **Verification**: `coro_json` example successfully running.

## 🚀 Phase 4: Async I/O & Performance Optimization (IN PROGRESS)
- [x] **Robust Event Handling**: Multi-event support (Read/Write) per FD in Kqueue.
- [x] **Awaiters**: Implemented `AsyncRead`, `AsyncWrite`, and `AsyncSleep`.
- [ ] **Debugging & Hardening**:
    - [ ] Resolve segfault in `AsyncSleep` resumption (Reactor callback ownership issue).
    - [ ] Robust lifecycle management for detached coroutines in `App::listen`.
- [ ] **Performance Verification**:
    - [ ] Zero-copy Serialization Optimizations for `Beast JSON`.
    - [ ] Benchmark Suite (Latency/Throughput comparisons).

## 🛡️ Future Phases: Middleware & Production Hardening
- [ ] **Middleware Pipeline**: Logging, CORS, Auth, and Error Handling.
- [ ] **TLS/SSL Support**: OpenSSL/BoringSSL non-blocking plugin.
- [ ] **C10M Goal**: Stress testing 10M+ concurrent connections.

## ⚠️ Known Issues & Improvement Areas
1.  **Beast JSON Mutation**: `operator[]` on `beast::Value` does not create keys. Use `.insert()` instead.
2.  **Reactor Callback Segfault**: Under investigation. Possible use-after-free when callbacks unregister themselves during invocation.
3.  **Task Return Value**: `Task<T>` needs robust testing for complex types.

## 📝 Contribution & Issues
- For bugs, create a GitHub Issue with a reproducible test case (C++ example).
- Performance profiling should use `macOS Performance Tools` or `Linux perf`.

---
*Created by The LKB Innovations.*
