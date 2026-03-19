# C++23 Upgrade & Refactoring Strategy

This document outlines the current state of C++23 adoption in `qbuem-stack` and identifies areas for further refactoring to fully leverage C++23 features.

## 1. Standard Error Handling: `std::expected`

The project is transitioning from custom aliases (`Result<T>`) to direct usage of the C++23 standard library error-handling types.

- **Status**: 🔄 In Transition (Moving away from `Result<T>` alias)
- **Rationale**: Using pure `std::expected<T, std::error_code>` and `std::unexpected` increases code transparency, ensures compliance with modern IDE/tooling expectations, and avoids "magic" types that hide standard implementation details.
- **Refactoring Goal**:
    - Replace all occurrences of `qbuem::Result<T>` with `std::expected<T, std::error_code>`.
    - Replace all occurrences of `qbuem::unexpected(e)` with `std::unexpected(e)`.
    - Use `std::expected<void, std::error_code>` for operations that previously returned `Result<void>`.
- **Implementation**: `include/qbuem/common.hpp` will eventually deprecate or remove the `Result` and `unexpected` aliases once the migration is complete.

## 2. Targeted C++23 Refactoring Opportunities

The following areas have been identified where C++23 features can replace older or non-standard patterns.

### 2.1. `std::print` and `std::println`
Replace legacy C-style `printf`/`fprintf` and `std::cout` with the type-safe and efficient `std::print` and `std::println`.

- **Current Status**: ❌ Legacy `printf`, `fprintf`, and `std::snprintf` are used in benchmarks, examples, and some library code.
- **Refactoring Goal**: 
    - Replace `printf(...)` and `fprintf(stderr, ...)` in `bench/`, `examples/`, and `include/qbuem/pipeline/observability.hpp`.
    - Remove `include/qbuem/compat/print.hpp` (the current shim) once a C++23 compliant compiler is guaranteed for all builds.
    - Replace `std::snprintf` with `std::format` or `std::format_to` where appropriate (while respecting hot-path constraints).

### 2.2. `std::jthread`
Replace `std::thread` with `std::jthread` for safer, RAII-based thread management (automatic joining on destruction).

- **Current Status**: ❌ `std::thread` is used in `Dispatcher`, `DnsResolver`, and several test files.
- **Refactoring Goal**:
    - Update `include/qbuem/core/dispatcher.hpp` to use `std::jthread`.
    - Update `include/qbuem/net/dns.hpp`.
    - Note: `std::jthread` also supports `std::stop_token` natively, which aligns with the project's cancellation strategy.

### 2.3. `std::to_underlying`
Replace `static_cast<UnderlyingType>(enum_value)` with the more semantic `std::to_underlying()`.

- **Current Status**: ❌ Manual `static_cast` is used in `WebSocketHandler` and `Numa` logic.
- **Refactoring Goal**:
    - `include/qbuem/server/websocket_handler.hpp`: Replace `static_cast<uint8_t>(frame.opcode)`.
    - `include/qbuem/core/numa.hpp`: Replace `static_cast<int>(thread_count)`.

### 2.4. `if consteval`
Replace `if (std::is_constant_evaluated())` with the C++23 `if consteval` syntax for clearer compile-time branching.

- **Current Status**: ℹ️ Potential usage in `simd_erasure.hpp` or `simd_validator.hpp` for fallback logic.

### 2.5. `std::views` and `std::ranges`
Leverage C++23 range improvements such as `std::views::zip`, `std::views::enumerate`, and `std::views::chunk`.

- **Refactoring Goal**:
    - **Pipeline Batching**: Use `std::views::chunk` to process messages in fixed-size batches.
    - **Multi-vector Iteration**: Use `std::views::zip` in `simd_erasure.hpp` or `dynamic_router.hpp` when iterating over multiple aligned buffers simultaneously.
    - **SIMD Loops**: Use `std::views::enumerate` to get indices during manual vectorization fallbacks.

### 2.6. `std::mdspan`
For multi-dimensional data like image frames in `sensor_fusion` or 3D grids in `high-performance-primitives.md`.

- **Refactoring Goal**:
    - Explore `std::mdspan` for `include/qbuem/pipeline/sensor_fusion.hpp` to provide a standard view over multi-channel raw data.

### 2.7. `auto(x)` and `auto{x}` (Explicit decay-copy)
Use `auto(x)` when a copy is explicitly intended, such as passing a value to a coroutine or a capture-by-copy lambda.

## 3. Implementation Roadmap

1. **Phase 1 (Low Risk)**: Replace `printf` with `std::print` in examples and benchmarks.
2. **Phase 2 (Semantic)**: Replace `static_cast` on enums with `std::to_underlying`.
3. **Phase 3 (Core)**: Migrate `std::thread` to `std::jthread` in `Dispatcher` and `Reactor` management.
4. **Phase 4 (Advanced)**: Integrate `std::views` (chunk, zip) into pipeline and SIMD processing loops.
