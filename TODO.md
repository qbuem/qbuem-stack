# Draco WAS Development Roadmap (v0.2.0)

Draco WAS is a C++20/23 ultra-low latency Web Application Server designed for extreme performance and zero-dependency agility.

## ✅ Phase 0-2 & L: Foundation & Core Engine (COMPLETED)
- [x] Expert architecture design (Shared-Nothing, io_uring, Coroutines).
- [x] C++23 project foundation & directory structure.
- [x] Dependency management via `FetchContent` (Beast-JSON, GTest).
- [x] **Reactor Core**: `kqueue` (macOS) and `epoll` (Linux) implemented.
- [x] **Dispatcher**: Thread-per-core Dispatcher with CPU affinity support.
- [x] **HTTP Abstractions**: Zero-copy HTTP Parser, `Request`/`Response` API.
- [x] **Modern Routing**: Radix Tree for fast route matching.
- [x] **Library Infrastructure**: Refactored to header + source distribution with modern CMake.

## ✅ Phase 3: Coroutines & Beast JSON (COMPLETED)
- [x] **Async Flow**: Custom `draco::Task<T>` with symmetric transfer for nested coroutines.
- [x] **Beast JSON Integration**: Native integration in `Request`/`Response`.
- [x] **Verification**: `coro_json` example successfully running.

## ✅ Phase 4: Async I/O & Hardening (COMPLETED)
- [x] **Robust Event Handling**: Multi-event support (Read/Write) per FD.
- [x] **Awaiters**: `AsyncRead`, `AsyncWrite`, and `AsyncSleep` (timer-based).
- [x] **Linux Reactor**: `EpollReactor` using `epoll` + `timerfd` replaces the io_uring stub.
- [x] **Callback Safety**: Callbacks are copied before invocation in all reactors, preventing UAF when a callback unregisters itself.
- [x] **Detached Coroutine Lifecycle**: `Task::detach()` now sets a flag so the coroutine self-destructs on completion, eliminating the memory leak for fire-and-forget async handlers.
- [x] **Async Handler Hardening**: `App::listen` async wrapper uses the corrected `detach()` semantics; detached frames are automatically cleaned up.
- [x] **Build Fixed on Linux**: Platform-specific reactor compilation is now conditional (`kqueue_reactor` on macOS, `epoll_reactor` on Linux).
- [x] **Test Suite Expanded**: `http_test.cpp` added to the test runner; `RouterTest` updated to work correctly with `HandlerVariant`.

## 🛡️ Future Phases: Middleware & Production Hardening
- [ ] **Middleware Pipeline**: Logging, CORS, Auth, and Error Handling.
- [ ] **TLS/SSL Support**: OpenSSL/BoringSSL non-blocking plugin.
- [ ] **Zero-copy Serialization**: Beast JSON write optimizations.
- [ ] **Benchmark Suite**: Latency/Throughput comparisons (wrk, hey).
- [ ] **C10M Goal**: Stress testing 10M+ concurrent connections.

## 📝 Contribution & Issues
- For bugs, create a GitHub Issue with a reproducible test case (C++ example).
- Performance profiling: use `Linux perf` or `macOS Instruments`.

---
*Created by The LKB Innovations.*
