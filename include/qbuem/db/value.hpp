#pragma once

/**
 * @file qbuem/db/value.hpp
 * @brief db::Value — Heap-allocation-free DB parameter/result binding type.
 * @defgroup qbuem_db_value db::Value
 * @ingroup qbuem_db
 *
 * `db::Value` is a heap-free variant type used for SQL parameter binding
 * and result extraction.
 *
 * ## Supported Types
 * - `Null`    : SQL NULL
 * - `int64_t` : Integer (8 bytes)
 * - `double`  : Floating point
 * - `bool`    : Boolean
 * - `StringView` : String view (zero-copy, references external buffer)
 * - `BlobView`   : Binary data view (zero-copy)
 *
 * ## Design Principles
 * - **Zero Allocation**: No heap memory is used at all.
 *   Strings and blobs are referenced as views (span) without ownership.
 * - **Cache-Friendly**: sizeof(Value) == 24B (pointer + length + tag).
 *
 * @code
 * db::Value v1 = db::null;
 * db::Value v2 = int64_t{42};
 * db::Value v3 = 3.14;
 * db::Value v4 = std::string_view{"hello"};
 *
 * if (v2.is<int64_t>()) {
 *     int64_t n = v2.get<int64_t>();
 * }
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace qbuem::db {

// ─── Null sentinel ────────────────────────────────────────────────────────────

/** @brief Tag type representing SQL NULL. */
struct Null {
    constexpr bool operator==(Null) const noexcept { return true; }
};

/** @brief SQL NULL singleton. */
inline constexpr Null null{};

// ─── Value ────────────────────────────────────────────────────────────────────

/**
 * @brief Heap-allocation-free DB parameter / result binding type.
 *
 * Uses a union + type tag internally, so no heap allocation occurs.
 * Strings and blobs store only a reference (view); lifetime management
 * is the caller's responsibility.
 */
class Value {
public:
    /** @brief List of supported types. */
    enum class Type : uint8_t {
        Null    = 0,
        Int64   = 1,
        Float64 = 2,
        Bool    = 3,
        Text    = 4,  ///< UTF-8 string (zero-copy view)
        Blob    = 5,  ///< Binary data (zero-copy view)
    };

    // ── Constructors ────────────────────────────────────────────────────────

    /** @brief Default initialization to NULL. */
    constexpr Value() noexcept : type_(Type::Null), i64_(0) {}

    /** @brief Initializes to NULL. */
    constexpr Value(Null) noexcept : type_(Type::Null), i64_(0) {} // NOLINT

    /** @brief Initializes with an integer value. */
    constexpr Value(int64_t v) noexcept : type_(Type::Int64), i64_(v) {} // NOLINT

    /** @brief Initializes by converting int32 to int64. */
    constexpr Value(int32_t v) noexcept : type_(Type::Int64), i64_(v) {} // NOLINT

    /** @brief Initializes with a floating-point value. */
    constexpr Value(double v) noexcept : type_(Type::Float64), f64_(v) {} // NOLINT

    /** @brief Initializes by converting float to double. */
    constexpr Value(float v) noexcept : type_(Type::Float64), f64_(v) {} // NOLINT

    /** @brief Initializes with a boolean value. */
    constexpr Value(bool v) noexcept : type_(Type::Bool), i64_(v ? 1 : 0) {} // NOLINT

    /** @brief Initializes with a string view (zero-copy). Caller is responsible for lifetime. */
    constexpr Value(std::string_view sv) noexcept // NOLINT
        : type_(Type::Text), text_(sv) {}

    /** @brief Initializes with a binary data view (zero-copy). */
    constexpr Value(BufferView bv) noexcept // NOLINT
        : type_(Type::Blob), blob_(bv) {}

    // ── Type checks ─────────────────────────────────────────────────────────

    /** @brief Returns the currently stored type. */
    [[nodiscard]] constexpr Type type() const noexcept { return type_; }

    /** @brief Checks whether the value is NULL. */
    [[nodiscard]] constexpr bool is_null() const noexcept {
        return type_ == Type::Null;
    }

    /**
     * @brief Checks whether the value holds a specific type.
     * @tparam T Type to check (int64_t, double, bool, std::string_view, BufferView, Null).
     */
    template <typename T>
    [[nodiscard]] constexpr bool is() const noexcept {
        if constexpr (std::is_same_v<T, Null>)
            return type_ == Type::Null;
        else if constexpr (std::is_same_v<T, int64_t>)
            return type_ == Type::Int64;
        else if constexpr (std::is_same_v<T, double>)
            return type_ == Type::Float64;
        else if constexpr (std::is_same_v<T, bool>)
            return type_ == Type::Bool;
        else if constexpr (std::is_same_v<T, std::string_view>)
            return type_ == Type::Text;
        else if constexpr (std::is_same_v<T, BufferView>)
            return type_ == Type::Blob;
        else
            return false;
    }

    // ── Value extraction ────────────────────────────────────────────────────

    /**
     * @brief Extracts the stored value.
     * @tparam T Type to extract.
     * @returns The stored value.
     * @note UB if the type does not match — always call `is<T>()` first.
     */
    template <typename T>
    [[nodiscard]] constexpr T get() const noexcept {
        if constexpr (std::is_same_v<T, int64_t>)   return i64_;
        else if constexpr (std::is_same_v<T, double>)return f64_;
        else if constexpr (std::is_same_v<T, bool>)  return i64_ != 0;
        else if constexpr (std::is_same_v<T, std::string_view>) return text_;
        else if constexpr (std::is_same_v<T, BufferView>) return blob_;
        else static_assert(sizeof(T) == 0, "Unsupported type for db::Value::get<T>()");
    }

    // ── Comparison operators ────────────────────────────────────────────────

    [[nodiscard]] bool operator==(const Value& o) const noexcept {
        if (type_ != o.type_) return false;
        switch (type_) {
            case Type::Null:    return true;
            case Type::Int64:   return i64_  == o.i64_;
            case Type::Float64: return f64_  == o.f64_;
            case Type::Bool:    return i64_  == o.i64_;
            case Type::Text:    return text_  == o.text_;
            case Type::Blob:    return blob_.data() == o.blob_.data()
                                       && blob_.size() == o.blob_.size();
        }
        return false;
    }

    [[nodiscard]] bool operator!=(const Value& o) const noexcept {
        return !(*this == o);
    }

private:
    Type type_;

    union {
        int64_t      i64_;
        double       f64_;
        std::string_view text_;
        BufferView   blob_;
    };
};

static_assert(sizeof(Value) <= 32, "db::Value must fit in 32 bytes");

// ─── Params ───────────────────────────────────────────────────────────────────

/**
 * @brief Fixed-size array for parameter binding.
 *
 * Stack allocation optimization: small numbers of parameters (≤8) are stored
 * on the stack without heap allocation.
 */
template <size_t N = 8>
struct BoundParams {
    std::array<Value, N> values{};
    uint8_t count{0};

    void bind(Value v) {
        if (count < N) values[count++] = v;
    }

    [[nodiscard]] std::span<const Value> span() const noexcept {
        return {values.data(), count};
    }
};

} // namespace qbuem::db

/** @} */
