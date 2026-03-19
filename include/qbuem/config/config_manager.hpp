#pragma once

/**
 * @file qbuem/config/config_manager.hpp
 * @brief Secure, high-performance configuration management module.
 * @defgroup qbuem_config Secure Configuration
 * @ingroup qbuem
 *
 * Provides hierarchical, zero-allocation-at-access configuration with:
 * - `Secret<T>`     : Move-only sensitive value wrapper (RAII memory zeroing,
 *                     masked std::format/std::print output, explicit reveal()).
 * - `ConfigValue`   : Heap-free tagged union (int64, double, bool, string≤255B).
 * - `ConfigManager` : Layered loader (defaults → file → env → explicit set).
 *                     Immutable post-init: all access is lock-free read-only.
 *
 * ### Reference
 * [docs/secure-config-architecture.md](../../../../docs/secure-config-architecture.md)
 *
 * ### C++23 Features Used
 * - `std::expected<T, E>` for all fallible operations
 * - `std::string_view` for zero-copy key lookups
 * - `if consteval` / `if constexpr` for compile-time dispatch
 * - `std::to_underlying` for ConfigValue::Type conversions
 * - `std::format` for diagnostic messages
 *
 * ### Design Constraints (Pillar 3 — Zero Allocation)
 * - `get_or()` / `contains()` are O(1), lock-free, zero-heap after init.
 * - Internal storage is a fixed-capacity open-addressing flat table
 *   (`alignas(64)` entries, no heap allocation at access time).
 * - `load_*()` and `set*()` are init-time only (call before serving requests).
 *
 * @{
 */

#include <qbuem/common.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace qbuem::config {

// ─── Secret<T> ────────────────────────────────────────────────────────────────

/**
 * @brief Move-only sensitive value wrapper with RAII memory zeroing.
 *
 * Prevents accidental logging and in-memory persistence of secrets:
 * - **Non-copyable** — secrets cannot be silently duplicated.
 * - `std::format`/`std::print` always emit `[REDACTED]` (via `std::formatter`
 *   specialisation below).
 * - Destructor calls a `volatile` byte-clear to prevent dead-store elimination.
 * - `reveal()` requires explicit opt-in — forces intentional, auditable usage.
 *
 * ### Example
 * ```cpp
 * Secret<std::string> key{"s3cr3t-api-key"};
 * std::println("{}", key);          // prints: [REDACTED]
 * make_api_call(key.reveal());      // explicit opt-in
 * ```
 *
 * @tparam T  A string-like type supporting `.data()` and `.size()`
 *            (e.g., `std::string`, `std::array<char, N>`).
 */
template <typename T>
class Secret {
public:
    static_assert(
        requires(T t) { t.data(); t.size(); },
        "Secret<T> requires a string-like type with .data() and .size()");

    Secret() = default;
    explicit Secret(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
        : value_(std::move(value)) {}

    // Non-copyable
    Secret(const Secret&)            = delete;
    Secret& operator=(const Secret&) = delete;

    Secret(Secret&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
        : value_(std::move(other.value_)) {
        other.wipe();
    }
    Secret& operator=(Secret&& other) noexcept(std::is_nothrow_move_assignable_v<T>) {
        if (this != &other) {
            wipe();
            value_ = std::move(other.value_);
            other.wipe();
        }
        return *this;
    }

    ~Secret() noexcept { wipe(); }

    /**
     * @brief Returns a const reference to the underlying secret value.
     *
     * Intentionally verbose name — every call site is auditable.
     * Do **not** store the returned reference beyond the immediate call.
     */
    [[nodiscard]] const T& reveal() const noexcept { return value_; }

    /** @brief True if the secret holds a non-empty value. */
    [[nodiscard]] bool has_value() const noexcept {
        if constexpr (requires { value_.empty(); }) return !value_.empty();
        else return true;
    }

private:
    T value_{};

    /// Volatile byte-clear prevents dead-store elimination.
    void wipe() noexcept {
        if constexpr (requires { value_.data(); value_.size(); }) {
            volatile char* p = reinterpret_cast<volatile char*>(
                const_cast<char*>(reinterpret_cast<const char*>(value_.data())));
            const size_t n = value_.size();
            for (size_t i = 0; i < n; ++i) p[i] = '\0';
        }
    }
};

} // namespace qbuem::config

// ─── std::formatter specialisation — always prints "[REDACTED]" ───────────────
template <typename T>
struct std::formatter<qbuem::config::Secret<T>> : std::formatter<std::string_view> {
    template <typename FormatContext>
    auto format(const qbuem::config::Secret<T>&, FormatContext& ctx) const {
        return std::formatter<std::string_view>::format("[REDACTED]", ctx);
    }
};

namespace qbuem::config {

// ─── ConfigValue ──────────────────────────────────────────────────────────────

/**
 * @brief Heap-free tagged union for a single configuration value.
 *
 * Supports four primitive types used in typical config schemas.
 * Inline string storage (≤255 bytes) avoids heap allocation at access time.
 * Strings exceeding the inline buffer are truncated with a null terminator.
 *
 * Access via the wrong `as_*()` overload returns a safe default (0 / false / "").
 */
class ConfigValue {
public:
    enum class Type : uint8_t {
        Unset  = 0,
        Int    = 1,
        Double = 2,
        Bool   = 3,
        String = 4,
    };

    ConfigValue() noexcept = default;

    explicit ConfigValue(int64_t v) noexcept  : type_(Type::Int)    { u_.i = v; }
    explicit ConfigValue(double  v) noexcept  : type_(Type::Double) { u_.d = v; }
    explicit ConfigValue(bool    v) noexcept  : type_(Type::Bool)   { u_.b = v; }

    explicit ConfigValue(std::string_view sv) noexcept : type_(Type::String) {
        const size_t n = std::min(sv.size(), kInlineMax);
        std::memcpy(u_.s, sv.data(), n);
        u_.s[n] = '\0';
    }

    [[nodiscard]] Type type()   const noexcept { return type_; }
    [[nodiscard]] bool is_set() const noexcept { return type_ != Type::Unset; }

    [[nodiscard]] int64_t        as_int()    const noexcept { return type_ == Type::Int    ? u_.i   : 0; }
    [[nodiscard]] double         as_double() const noexcept { return type_ == Type::Double ? u_.d   : 0.0; }
    [[nodiscard]] bool           as_bool()   const noexcept { return type_ == Type::Bool   ? u_.b   : false; }
    [[nodiscard]] std::string_view as_string() const noexcept {
        return type_ == Type::String ? std::string_view{u_.s} : std::string_view{};
    }

private:
    static constexpr size_t kInlineMax = 255;

    Type type_{Type::Unset};
    union {
        int64_t i;
        double  d;
        bool    b;
        char    s[256]; // null-terminated inline string (≤255 bytes + NUL)
    } u_{};
};

// ─── Detail: FNV-1a key hash ──────────────────────────────────────────────────

namespace detail {

/// 64-bit FNV-1a hash (no heap, constexpr-compatible).
[[nodiscard]] inline constexpr uint64_t fnv1a(std::string_view key) noexcept {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : key) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ULL;
    }
    return h == 0 ? 1 : h; // 0 is reserved as "empty" sentinel
}

} // namespace detail

// ─── ConfigTable — fixed flat open-addressing hash table ─────────────────────

/**
 * @brief Fixed-capacity open-addressing flat hash table for ConfigValue.
 *
 * Not thread-safe for writes; all reads are safe after the init phase.
 * Designed for ConfigManager internals — not part of the public API.
 *
 * @tparam Cap  Slot capacity; must be power of two. Default: 512.
 */
template <size_t Cap = 512>
class ConfigTable {
    static_assert((Cap & (Cap - 1)) == 0, "Cap must be a power of two");
    static constexpr uint64_t kEmpty = 0;

    struct alignas(64) Slot {
        uint64_t    key{kEmpty};
        ConfigValue value{};
    };

public:
    ConfigTable() noexcept { entries_.fill(Slot{}); }

    /**
     * @brief Insert or update a key-value pair.
     * @returns false if the table is full.
     */
    bool insert_or_assign(uint64_t key, const ConfigValue& val) noexcept {
        size_t idx = key & (Cap - 1);
        for (size_t i = 0; i < Cap; ++i) {
            Slot& s = entries_[(idx + i) & (Cap - 1)];
            if (s.key == kEmpty || s.key == key) {
                s.key   = key;
                s.value = val;
                return true;
            }
        }
        return false; // table full
    }

    /**
     * @brief Only insert if the key does not already exist.
     */
    bool insert_if_absent(uint64_t key, const ConfigValue& val) noexcept {
        size_t idx = key & (Cap - 1);
        for (size_t i = 0; i < Cap; ++i) {
            Slot& s = entries_[(idx + i) & (Cap - 1)];
            if (s.key == key)   return false; // already present
            if (s.key == kEmpty) {
                s.key   = key;
                s.value = val;
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Look up a key.
     * @returns Pointer to the stored ConfigValue, or nullptr.
     */
    [[nodiscard]] const ConfigValue* find(uint64_t key) const noexcept {
        size_t idx = key & (Cap - 1);
        for (size_t i = 0; i < Cap; ++i) {
            const Slot& s = entries_[(idx + i) & (Cap - 1)];
            if (s.key == kEmpty) return nullptr;
            if (s.key == key)    return &s.value;
        }
        return nullptr;
    }

    /** @brief Remove a key (marks slot as empty — safe during init only). */
    void erase(uint64_t key) noexcept {
        size_t idx = key & (Cap - 1);
        for (size_t i = 0; i < Cap; ++i) {
            Slot& s = entries_[(idx + i) & (Cap - 1)];
            if (s.key == kEmpty) return;
            if (s.key == key) { s.key = kEmpty; s.value = ConfigValue{}; return; }
        }
    }

private:
    std::array<Slot, Cap> entries_{};
};

// ─── ConfigManager ────────────────────────────────────────────────────────────

/**
 * @brief Zero-allocation-at-access hierarchical configuration manager.
 *
 * ### Loading Priority (lowest → highest)
 * 1. Embedded defaults (`set_default()`)
 * 2. Config file  (`load_file()` — `KEY=VALUE` text format)
 * 3. Environment variables (`load_env()` — prefix-filtered, normalised)
 * 4. Explicit overrides (`set()`)
 *
 * ### Thread Safety
 * - `load_*()` and `set*()` are **NOT** thread-safe — call during app init only.
 * - `get_or()`, `get_secret()`, and `contains()` are **thread-safe** after
 *   the init phase (purely read-only accesses to the immutable flat table).
 *
 * ### Usage Example
 * ```cpp
 * namespace cfg = qbuem::config;
 * cfg::ConfigManager cm;
 * cm.set_default("server.port",    cfg::ConfigValue{int64_t{8080}});
 * cm.set_default("server.workers", cfg::ConfigValue{int64_t{4}});
 * cm.load_env("QBUEM_");        // QBUEM_SERVER_PORT=9090 overrides default
 * cm.load_file("config.ini");   // KEY=VALUE pairs override env
 *
 * auto port    = cm.get_or("server.port", int64_t{8080});
 * auto api_key = cm.get_secret("external.api_key");
 * if (api_key) connect(api_key->reveal());
 * ```
 */
class ConfigManager {
public:
    ConfigManager() = default;

    // Non-copyable (owns a large flat table on the stack/heap)
    ConfigManager(const ConfigManager&)            = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;
    ConfigManager(ConfigManager&&)                 = default;
    ConfigManager& operator=(ConfigManager&&)      = default;

    // ─── Init-time setters (NOT thread-safe) ─────────────────────────────────

    /**
     * @brief Register a default value (lowest priority).
     *
     * Will **not** overwrite an existing entry (e.g., from a prior `load_env()`).
     */
    void set_default(std::string_view key, ConfigValue value) noexcept {
        table_.insert_if_absent(detail::fnv1a(key), value);
    }

    /**
     * @brief Set or override a value (highest priority).
     *
     * Always overwrites any existing entry for this key.
     */
    void set(std::string_view key, ConfigValue value) noexcept {
        table_.insert_or_assign(detail::fnv1a(key), value);
    }

    /**
     * @brief Load environment variables with the given prefix.
     *
     * Variable names are normalised:
     * - Prefix is stripped.
     * - Remaining name is lowercased; `_` is replaced with `.`.
     *
     * Example: `QBUEM_SERVER_PORT=9090` with prefix `"QBUEM_"` → key `"server.port"`.
     *
     * All env-loaded values are stored as strings; `get_or<int64_t>()` will
     * parse them on demand.
     *
     * @param prefix  Case-sensitive env var prefix (e.g., `"QBUEM_"`).
     */
    [[nodiscard]] std::expected<void, std::error_code>
    load_env(std::string_view prefix) noexcept {
        extern char** environ; // POSIX standard
        if (!environ) return {};

        const size_t plen = prefix.size();
        for (char** ep = environ; *ep; ++ep) {
            std::string_view entry{*ep};
            if (entry.size() <= plen) continue;
            if (entry.substr(0, plen) != prefix) continue;

            const auto eq = entry.find('=', plen);
            if (eq == std::string_view::npos) continue;

            std::string_view raw_key = entry.substr(plen, eq - plen);
            std::string_view val_str = entry.substr(eq + 1);

            // Normalize: lowercase + _ → .
            char key_buf[256]{};
            const size_t klen = std::min(raw_key.size(), sizeof(key_buf) - 1);
            for (size_t i = 0; i < klen; ++i) {
                const char c = raw_key[i];
                key_buf[i] = (c == '_') ? '.'
                           : (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32)
                           : c;
            }
            table_.insert_or_assign(detail::fnv1a(std::string_view{key_buf, klen}),
                                    ConfigValue{val_str});
        }
        return {};
    }

    /**
     * @brief Load config from a `KEY=VALUE` text file.
     *
     * - Blank lines and lines starting with `#` are ignored.
     * - Keys are lowercased; whitespace around `=` is stripped.
     * - File values override any previously loaded values.
     *
     * @param path  Path to the config file.
     * @returns Error if the file cannot be opened.
     */
    [[nodiscard]] std::expected<void, std::error_code>
    load_file(std::string_view path) noexcept {
        char path_buf[4096]{};
        const size_t plen = std::min(path.size(), sizeof(path_buf) - 1);
        std::memcpy(path_buf, path.data(), plen);

        FILE* f = ::fopen(path_buf, "r"); // NOLINT(cppcoreguidelines-owning-memory)
        if (!f)
            return std::unexpected(
                std::error_code{errno, std::system_category()});

        char line[1024]{};
        while (::fgets(line, sizeof(line), f)) {
            std::string_view sv{line};
            while (!sv.empty() && (sv.back() == '\n' || sv.back() == '\r'))
                sv.remove_suffix(1);
            while (!sv.empty() && sv.front() == ' ') sv.remove_prefix(1);
            if (sv.empty() || sv.front() == '#') continue;

            const auto eq = sv.find('=');
            if (eq == std::string_view::npos) continue;

            std::string_view raw_key = sv.substr(0, eq);
            std::string_view val_str = sv.substr(eq + 1);

            while (!raw_key.empty() && raw_key.back() == ' ')  raw_key.remove_suffix(1);
            while (!val_str.empty() && val_str.front() == ' ') val_str.remove_prefix(1);

            char key_buf[256]{};
            const size_t klen = std::min(raw_key.size(), sizeof(key_buf) - 1);
            for (size_t i = 0; i < klen; ++i) {
                const char c = raw_key[i];
                key_buf[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
            }

            table_.insert_or_assign(detail::fnv1a(std::string_view{key_buf, klen}),
                                    ConfigValue{val_str});
        }
        ::fclose(f);
        return {};
    }

    // ─── Hot-path read accessors (thread-safe, lock-free) ────────────────────

    /**
     * @brief Get a typed config value, or return `default_val`.
     *
     * Performs an O(1) hash lookup; returns `default_val` if the key is not
     * found or if the stored type cannot be coerced to `T`.
     *
     * Supported types for `T`: `int64_t`, `double`, `bool`, `std::string_view`.
     *
     * String→int/double/bool coercion is performed for values loaded from
     * environment variables or config files.
     *
     * @param key          Config key (case-sensitive, dot-separated).
     * @param default_val  Value returned when the key is absent.
     * @tparam T           One of: `int64_t`, `double`, `bool`, `std::string_view`.
     */
    template <typename T>
    [[nodiscard]] T get_or(std::string_view key, T default_val) const noexcept {
        const ConfigValue* v = table_.find(detail::fnv1a(key));
        if (!v) return default_val;

        if constexpr (std::is_same_v<T, int64_t>) {
            if (v->type() == ConfigValue::Type::Int)    return v->as_int();
            if (v->type() == ConfigValue::Type::String) {
                int64_t out{};
                const auto sv = v->as_string();
                auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
                if (ec == std::errc{}) return out;
            }
        } else if constexpr (std::is_same_v<T, double>) {
            if (v->type() == ConfigValue::Type::Double) return v->as_double();
            if (v->type() == ConfigValue::Type::String) {
                // strtod via a fixed stack buffer (no heap)
                const auto sv = v->as_string();
                char buf[64]{};
                const size_t n = std::min(sv.size(), sizeof(buf) - 1);
                std::memcpy(buf, sv.data(), n);
                char* end{};
                const double d = std::strtod(buf, &end);
                if (end != buf) return d;
            }
        } else if constexpr (std::is_same_v<T, bool>) {
            if (v->type() == ConfigValue::Type::Bool)   return v->as_bool();
            if (v->type() == ConfigValue::Type::String) {
                const auto sv = v->as_string();
                return sv == "true" || sv == "1" || sv == "yes" || sv == "on";
            }
        } else if constexpr (std::is_same_v<T, std::string_view>) {
            if (v->type() == ConfigValue::Type::String) return v->as_string();
        }

        return default_val;
    }

    /**
     * @brief Retrieve a sensitive value as `Secret<std::string>`.
     *
     * The internal map stores a plain string; this method wraps a copy in
     * `Secret<>` so the caller never accidentally logs the raw value.
     *
     * @param key  Config key.
     * @returns `Secret<std::string>` on success; error if not found or wrong type.
     */
    [[nodiscard]] std::expected<Secret<std::string>, std::error_code>
    get_secret(std::string_view key) const noexcept {
        const ConfigValue* v = table_.find(detail::fnv1a(key));
        if (!v)
            return std::unexpected(
                std::make_error_code(std::errc::no_such_file_or_directory));
        if (v->type() != ConfigValue::Type::String)
            return std::unexpected(
                std::make_error_code(std::errc::invalid_argument));
        return Secret<std::string>{std::string{v->as_string()}};
    }

    /**
     * @brief Returns true if the key is present in the config.
     */
    [[nodiscard]] bool contains(std::string_view key) const noexcept {
        return table_.find(detail::fnv1a(key)) != nullptr;
    }

    /**
     * @brief Remove a key entry (init-time use only).
     */
    void erase(std::string_view key) noexcept {
        table_.erase(detail::fnv1a(key));
    }

private:
    ConfigTable<512> table_;
};

} // namespace qbuem::config

/** @} */ // end of qbuem_config
