#pragma once

/**
 * @file qbuem/db/value.hpp
 * @brief db::Value — 힙 할당 없는 DB 파라미터/결과 바인딩 타입.
 * @defgroup qbuem_db_value db::Value
 * @ingroup qbuem_db
 *
 * `db::Value`는 SQL 파라미터 바인딩 및 결과 추출에 사용되는
 * 힙-프리 variant 타입입니다.
 *
 * ## 지원 타입
 * - `Null`    : SQL NULL
 * - `int64_t` : 정수 (8바이트)
 * - `double`  : 부동소수점
 * - `bool`    : 논리값
 * - `StringView` : 문자열 뷰 (zero-copy, 외부 버퍼 참조)
 * - `BlobView`   : 이진 데이터 뷰 (zero-copy)
 *
 * ## 설계 원칙
 * - **Zero Allocation**: 힙 메모리를 일절 사용하지 않습니다.
 *   문자열/블롭은 소유하지 않고 뷰(span)로 참조합니다.
 * - **Cache-Friendly**: sizeof(Value) == 24B (포인터 + 길이 + 태그).
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

#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace qbuem::db {

// ─── Null sentinel ────────────────────────────────────────────────────────────

/** @brief SQL NULL을 표현하는 태그 타입. */
struct Null {
    constexpr bool operator==(Null) const noexcept { return true; }
};

/** @brief SQL NULL 싱글톤. */
inline constexpr Null null{};

// ─── Value ────────────────────────────────────────────────────────────────────

/**
 * @brief 힙 할당 없는 DB 파라미터 / 결과 바인딩 타입.
 *
 * 내부적으로 union + 타입 태그를 사용하므로 힙 할당이 발생하지 않습니다.
 * 문자열과 블롭은 참조(뷰)만 저장하므로 수명 관리는 호출자의 책임입니다.
 */
class Value {
public:
    /** @brief 지원하는 타입 목록. */
    enum class Type : uint8_t {
        Null    = 0,
        Int64   = 1,
        Float64 = 2,
        Bool    = 3,
        Text    = 4,  ///< UTF-8 문자열 (zero-copy 뷰)
        Blob    = 5,  ///< 이진 데이터 (zero-copy 뷰)
    };

    // ── 생성자 ─────────────────────────────────────────────────────────────

    /** @brief NULL 값으로 기본 초기화. */
    constexpr Value() noexcept : type_(Type::Null), i64_(0) {}

    /** @brief NULL 값으로 초기화. */
    constexpr Value(Null) noexcept : type_(Type::Null), i64_(0) {} // NOLINT

    /** @brief 정수값으로 초기화. */
    constexpr Value(int64_t v) noexcept : type_(Type::Int64), i64_(v) {} // NOLINT

    /** @brief int32를 int64로 변환하여 초기화. */
    constexpr Value(int32_t v) noexcept : type_(Type::Int64), i64_(v) {} // NOLINT

    /** @brief 부동소수점값으로 초기화. */
    constexpr Value(double v) noexcept : type_(Type::Float64), f64_(v) {} // NOLINT

    /** @brief float를 double로 변환하여 초기화. */
    constexpr Value(float v) noexcept : type_(Type::Float64), f64_(v) {} // NOLINT

    /** @brief 논리값으로 초기화. */
    constexpr Value(bool v) noexcept : type_(Type::Bool), i64_(v ? 1 : 0) {} // NOLINT

    /** @brief 문자열 뷰로 초기화 (zero-copy). 수명 관리는 호출자 책임. */
    constexpr Value(std::string_view sv) noexcept // NOLINT
        : type_(Type::Text), text_(sv) {}

    /** @brief 이진 데이터 뷰로 초기화 (zero-copy). */
    constexpr Value(BufferView bv) noexcept // NOLINT
        : type_(Type::Blob), blob_(bv) {}

    // ── 타입 검사 ──────────────────────────────────────────────────────────

    /** @brief 현재 저장된 타입을 반환합니다. */
    [[nodiscard]] constexpr Type type() const noexcept { return type_; }

    /** @brief NULL인지 확인합니다. */
    [[nodiscard]] constexpr bool is_null() const noexcept {
        return type_ == Type::Null;
    }

    /**
     * @brief 특정 타입인지 확인합니다.
     * @tparam T 확인할 타입 (int64_t, double, bool, std::string_view, BufferView, Null).
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

    // ── 값 추출 ────────────────────────────────────────────────────────────

    /**
     * @brief 값을 추출합니다.
     * @tparam T 추출할 타입.
     * @returns 저장된 값.
     * @note 타입이 맞지 않으면 UB — 반드시 `is<T>()` 후 호출.
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

    // ── 비교 연산자 ────────────────────────────────────────────────────────

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
 * @brief 파라미터 바인딩을 위한 고정-크기 배열.
 *
 * 스택 할당 최적화: 소량 파라미터(≤8개)는 힙 없이 스택에 저장.
 */
template <size_t N = 8>
struct BoundParams {
    Value   values[N]{};
    uint8_t count{0};

    void bind(Value v) {
        if (count < N) values[count++] = v;
    }

    std::span<const Value> span() const noexcept {
        return {values, count};
    }
};

} // namespace qbuem::db

/** @} */
