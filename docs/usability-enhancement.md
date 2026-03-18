# Usability & API Refinement Report: Toward a World-Class UX

This report outlines the proposed API refactors for `qbuem-stack` to improve developer productivity and code readability while maintaining zero-overhead performance.

## 1. Fluent Pipeline Composition (Deduction-based)

### Current Problem
Users must explicitly specify the output type for every stage, leading to verbose and brittle code.
```cpp
auto p = PipelineBuilder<Raw>()
    .add<Parsed>(stage_parse)
    .add<Enriched>(stage_enrich)
    .build();
```

### Proposed Refactor: `qbuem::pipe` and `operator|`
Leverage C++20 template deduction and the Pipe operator for a more natural flow.
```cpp
// Automatic type deduction from Task<Result<Out>> return type
auto p = qbuem::pipe<Raw>()
       | stage_parse    // Deduces Parsed
       | stage_enrich   // Deduces Enriched
       | stage_validate // Deduces Validated
       ;
```
- **Implementation**: `operator|` will internally call a type-deducing `add()`.

---

## 2. Leveraging C++20 for API Excellence

We will mandate C++20 idioms throughout the stack to eliminate boilerplate and enhance safety.

### 2.1. Strict Concepts & Constraints
Replace cryptic template errors with clear, readable requirements.
```cpp
template <typename T>
concept PipelineStage = requires(T t, In input) {
    { t.process(input) } -> std::same_as<Task<Result<Out>>>;
};

// Error: "Type 'MyInvalidStage' does not satisfy concept 'PipelineStage'"
auto p = qbuem::pipe<Raw>() | MyInvalidStage{}; 
```

### 2.2. Standardized Cancellation (`std::stop_token`)
Actions will use the standard C++20 cancellation mechanism, allowing for unified shutdown across the entire stack.
```cpp
Task<Result<Out>> stage_long_running(In in, std::stop_token stop) {
    while (!stop.stop_requested()) {
        // ...
    }
}
```

### 2.3. Zero-Allocation Views (`std::span`)
Migrate all `BufferView` references to `std::span<std::byte>`, aligning with the standard and improving interoperability with other C++20 libraries.

### 2.4. Compile-Time Validation (`consteval`)
Use `consteval` for schema validation and Morton code pre-computation, moving errors from runtime to compile-time.
```cpp
static constexpr auto my_mask = qbuem::spatial::compute_morton_mask<3>(10, 10, 10);
```

### 2.5. Enhanced Observability (`std::source_location`)
Eliminate `LOG_INFO` macros. Use `std::source_location` for zero-overhead, macro-free logging that retains file/line/function info.

---

## 3. Automated Dependency Injection (DI)

### Current Problem
Actions must manually fetch services from `ActionEnv`, which is repetitive and hard to unit test without mocking the whole environment.
```cpp
Task<Result<Out>> stage_parse(In input, ActionEnv env) {
    auto& db = env.services.get<Database>(); 
    // ...
}
```

### Proposed Refactor: Signature-based Injection
Allow Actions to declare exactly what they need in their parameters.
```cpp
Task<Result<Out>> stage_parse(In input, Database& db, AuthService& auth) {
    // Pipeline automatically resolves db and auth from ServiceRegistry
    // ...
}
```
- **Implementation**: Use variadic template meta-programming to inspect function arguments and bind them to `env.services`.

---

## 3. Batteries-Included `App` Presets

### Current Problem
Setting up a standard secure API requires many `app.use()` calls.

### Proposed Refactor: Feature Bundles
```cpp
auto app = qbuem::App{};

// Binds CORS, RequestID, Gzip, and Structured Logging in one call
app.use_standard_api_stack({
    .allowed_origins = {"*"},
    .log_format = LogFormat::JSON
});
```

---

## 4. Unified Error Handling (Monadic Results)

### Current Problem
`Result<T>` is mainly used inside `co_await`, but chaining error-prone operations requires manual `if(!res)`.

### Proposed Refactor: Monadic Chaining
```cpp
co_return (co_await stage_parse(input))
    .and_then(stage_validate)
    .and_then(stage_enrich)
    .or_else([](auto err) { /* error logic */ });
```

---

## 5. SHM Global Discovery

### Current Problem
Manually managing string names like `"trading.raw"` across processes is error-prone.

### Proposed Refactor: Native Discovery
```cpp
// In Proc A
shm_bus.publish<RawOrder>(GlobalTopic::Orders, msg);

// In Proc B
auto sub = shm_bus.subscribe<RawOrder>(GlobalTopic::Orders);
```
- **Implementation**: A compile-time `enum` or a central `Registry` that maps symbols to SHM keys.

---

## Implementation Plan
These changes will be rolled out as **v2.4.x - v2.5.0** updates, ensuring backward compatibility while introducing the new fluent interfaces.
