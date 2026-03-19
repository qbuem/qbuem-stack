# Secure Configuration Architecture (qbuem-config)

This document specifies the design for a high-performance, security-first configuration management module for `qbuem-stack`.

## 1. Design Philosophy

- **Zero Hardcoding**: No secrets, credentials, or environment-specific values in the source code.
- **Immutable Post-Init**: Configuration is loaded once during the "Boot Phase" and remains immutable throughout the "Execution Phase" to ensure lock-free access.
- **Secret Isolation**: Sensitive data is wrapped in specialized types that prevent accidental logging, serialisation, or memory persistence.
- **Type Safety**: Config values are accessed via typed getters with compile-time checks or C++23 `std::expected` error handling.

## 2. Security Pillars

### 2.1. The Layered Loading Strategy (Priority)
Configuration is merged from multiple sources in the following order (highest priority last):
1. **Embedded Defaults**: Hardcoded safe defaults (e.g., internal buffer sizes).
2. **Config File**: Local files (e.g., `config.json`, `config.toml`). **Must be added to `.gitignore`.**
3. **Environment Variables**: Prefixed variables (e.g., `QBUEM_DB_PASSWORD`).
4. **Secret Manager/KMS**: Runtime injection from external providers (AWS Secrets Manager, HashiCorp Vault).

### 2.2. Secret<T> Wrapper
To prevent sensitive data from leaking into logs or core dumps:
- **Masked `operator<<`**: Overloaded to print `[REDACTED]` instead of the actual value.
- **Memory Zeroing**: Uses `explicit_bzero` or `SecureZeroMemory` in the destructor to wipe the secret from RAM.
- **Non-Copyable**: Move-only to track ownership and prevent redundant copies in memory.

### 2.3. Anti-Logging Policy
The `qbuem-logger` is prohibited from accepting `Secret<T>` types. Any attempt to log a secret result in a compilation error or a mandatory `[REDACTED]` string.

## 3. Technical Specification

### 3.1. Data Structure
The configuration is stored as a flat, cache-aligned array of `std::variant` or a specialized `ConfigNode` tree for hierarchical access, optimized for O(1) or O(log N) lookup.

### 3.2. Access Patterns
```cpp
// 1. Static Access (Recommended for hot-paths)
const auto& db_cfg = app.config().get<DatabaseConfig>("database");

// 2. Secret Retrieval
std::expected<Secret<std::string>, std::error_code> apiKey = 
    app.config().get_secret("external_api_key");
```

## 4. Implementation Details

### 4.1. The ConfigManager Interface
```cpp
namespace qbuem::config {

template <typename T>
class Secret {
public:
    explicit Secret(T value) : value_(std::move(value)) {}
    ~Secret() { 
        // Zero out memory if T is a string or buffer
        if constexpr (requires { value_.data(); value_.size(); }) {
            std::fill_n(static_cast<volatile char*>(value_.data()), value_.size(), 0);
        }
    }
    
    // Prohibit accidental printing
    friend std::ostream& operator<<(std::ostream& os, const Secret&) {
        return os << "[REDACTED]";
    }

    const T& reveal() const { return value_; } // Explicit opt-in to use value

private:
    T value_;
};

class ConfigManager {
public:
    // Load from file and override with ENV
    std::expected<void, std::error_code> load(std::string_view path);

    // Fast typed access
    template<typename T>
    T get_or(std::string_view key, T default_val) const noexcept;

    // Secure secret access
    std::expected<Secret<std::string>, std::error_code> 
    get_secret(std::string_view key) const noexcept;

private:
    // SIMD-accelerated internal storage (e.g., Flattened Hash Map)
    LockFreeHashMap<uint64_t, ConfigValue> values_;
};

} // namespace qbuem::config
```

## 5. Security Checklist for Developers

1. **Never** log the result of `secret.reveal()`.
2. **Never** store `Secret<T>` in long-lived global objects if they are not needed.
3. **Always** check `std::expected` return values when loading configuration.
4. **Ensure** `.env` and `*.config.json` are in the project's `.gitignore`.
5. **Use** environment variables for production credentials; use local files only for development.

## 6. Performance Impact
- **Loading**: ~1-5ms (SIMD-accelerated JSON/TOML parsing).
- **Access**: ~5-10ns (O(1) hash lookup with `string_view` keys).
- **Memory**: O(N) where N is the number of config keys.
