# 🐉 Draco WAS (Development)

**Draco WAS** is a C++23 ultra-low latency Web Application Server designed for modern REST APIs. It combines the raw performance of systems-level C++ with the extreme developer-friendly ergonomics of frameworks like Express.js.

## ✨ Philosophy: Zero-Dependency, Zero-Overhead

Draco WAS is built for high-performance environments (HFT, real-time analytics, high-traffic APIs) where every microsecond matters. We achieve this through:

1.  **C++20/23 Modernity**: Fully leveraging Coroutines (`co_await`), Symmetric Transfer in `Task` types, and `std::expected` for safe, zero-overhead async flow.
2.  **Shared-Nothing Reactor**: A thread-per-core architecture using `kqueue` (macOS) and `io_uring` (Linux) that eliminates locking and maximizes cache locality.
3.  **Beast JSON Integrated**: Built-in support for `beast-json` for ultra-fast, zero-copy JSON parsing and serialization.
4.  **Compiled Library Design**: Transitioned from header-only to a structured `header + static/shared library` for better distribution and build times.

## 🚀 Current Implementation Status

Draco is evolving rapidly. Currently:
- **Phase 1-4 Completed**: Core Reactor, Dispatcher, Router (Radix Tree), Coroutine `Task`, robust Async I/O with `io_uring` (Linux) / `kqueue` (macOS), and coroutine lifecycle hardening.
- **Beast JSON Integration**: Fully integrated into `Request` and `Response` objects. Safe `SafeValue` access via `.get()` supported.
- **Async Awaiters**: `AsyncRead`, `AsyncWrite`, and `AsyncSleep` (timer-based) are available.

## 🛠 Features Roadmap

*   **Phase 5 (Next)**: Zero-copy Serialization Optimizations and C10M stress testing.

## 📦 Build Instructions

```bash
mkdir build && cd build
cmake ..
make -j$(sysctl -n hw.ncpu)
```

### Run Examples
```bash
./examples/hello_world
./examples/coro_json
./examples/async_timer
```

## ⚠️ Known Issues & Technical Gotchas

- **Beast JSON Mutations**: To add a new key to a `beast::Value` object, use `.insert("key", value)`. `operator[]` on a non-existent key returns an invalid `Value{}` — since beast-json v0.x (fix #61), assigning through it is a safe silent no-op rather than a segfault, but the key is still **not** inserted. Use `.get("key")` (returns `beast::SafeValue`) for safe optional-propagating read access on untrusted data.
- **Reactor Callbacks**: Avoid recursive event registration within the same callback without careful state management.

## 📦 Build Requirements

*   **Compiler**: GCC 13+ or Clang 16+ (C++20/23 required).
*   **OS**: Linux (io_uring) or macOS (kqueue).
*   **Windows**: Not supported.

---
*Created by The LKB Innovations.*
