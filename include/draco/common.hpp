#pragma once

/**
 * @file draco/common.hpp
 * @brief qbuem-stack 전역 공통 타입 정의.
 * @ingroup qbuem_common
 *
 * 이 헤더는 라이브러리 전반에서 사용되는 핵심 타입들을 정의합니다:
 *
 * - `unexpected<E>` : 에러를 명시적으로 표현하는 태그 타입 (C++23 std::unexpected 미러)
 * - `Result<T>`     : 성공값 또는 `std::error_code`를 담는 합산 타입 (C++23 std::expected 대체)
 * - `Result<void>`  : 반환값 없이 성공/실패만 표현하는 특수화
 * - `BufferView`    : 읽기 전용 바이트 뷰 (zero-copy, std::span 기반)
 * - `MutableBufferView` : 쓰기 가능한 바이트 뷰
 *
 * ### C++20 vs C++23 컨텍스트
 * qbuem-stack은 C++20을 기준으로 빌드됩니다.
 * C++23에서 표준화된 `std::expected<T, E>`와 `std::unexpected<E>`는
 * C++20 환경에서 사용할 수 없으므로, 이 헤더에서 동등한 기능을 직접 구현합니다.
 * 향후 C++23으로 마이그레이션할 경우 이 타입들을 표준 타입으로 교체할 수 있습니다.
 */

/**
 * @defgroup qbuem_common Common Types
 * @brief 라이브러리 전반에서 사용되는 공통 타입 및 유틸리티.
 *
 * 모든 공개 API는 이 그룹에서 정의된 `Result<T>` 타입을 통해
 * 에러를 전달합니다. 예외(exception)를 사용하지 않으므로
 * 핫 패스(hot path)에서의 예외 오버헤드가 없습니다.
 * @{
 */

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

namespace draco {

// ─── unexpected<E> ───────────────────────────────────────────────────────────

/**
 * @brief 에러값을 명시적으로 표현하는 태그 타입.
 *
 * C++23의 `std::unexpected<E>`를 C++20 환경에서 미러링합니다.
 * `Result<T>`의 에러 생성자를 호출할 때 성공 경로와 명확히 구분하기 위해 사용됩니다.
 *
 * 직접 구성하기보다는 아래 패턴을 사용하세요:
 * @code
 * // 함수 반환 시 에러를 명시적으로 표현
 * Result<int> read_byte(int fd) {
 *     if (fd < 0)
 *         return unexpected(std::make_error_code(std::errc::bad_file_descriptor));
 *     // ...
 * }
 * @endcode
 *
 * @tparam E 에러 값의 타입. 주로 `std::error_code`를 사용합니다.
 */
template <typename E>
struct unexpected {
  /** @brief 래핑된 에러 값. */
  E value;

  /**
   * @brief 에러 값으로 unexpected를 구성합니다.
   * @param e 래핑할 에러 값. 이동(move)됩니다.
   */
  explicit unexpected(E e) : value(std::move(e)) {}
};

/**
 * @brief `unexpected<E>` 추론 가이드.
 *
 * 템플릿 인수를 명시하지 않아도 컴파일러가 타입을 추론할 수 있게 합니다.
 * @code
 * auto u = unexpected(std::make_error_code(std::errc::timed_out)); // E 명시 불필요
 * @endcode
 */
template <typename E> unexpected(E) -> unexpected<E>;

// ─── Result<T> ───────────────────────────────────────────────────────────────

/**
 * @brief 성공값 `T` 또는 `std::error_code`를 담는 합산 타입.
 *
 * C++20 환경에서 C++23 `std::expected<T, std::error_code>`의 역할을 수행합니다.
 * 예외를 사용하지 않고 에러를 값으로 전달하는 패턴을 지원합니다.
 *
 * ### 상태 전이
 * - 성공 상태: `T` 값을 보유. `has_value()` == true, `operator bool()` == true.
 * - 에러 상태: `std::error_code`를 보유. `has_value()` == false.
 * - 초기 상태(private default ctor): monostate — 내부 빌더 메서드에서만 사용됩니다.
 *
 * ### 사용 예시
 * @code
 * Result<int> parse_port(std::string_view s) {
 *     int port = 0;
 *     // ... 파싱 로직 ...
 *     if (port < 0 || port > 65535)
 *         return unexpected(std::make_error_code(std::errc::invalid_argument));
 *     return port; // 암묵적으로 Result<int>::ok(port) 호출
 * }
 *
 * auto r = parse_port("8080");
 * if (!r) {
 *     std::cerr << "에러: " << r.error().message() << '\n';
 *     return;
 * }
 * std::cout << "포트: " << *r << '\n';
 * @endcode
 *
 * @tparam T 성공 시 보유할 값의 타입.
 *
 * @note `void` 특수화는 별도로 제공됩니다 (`Result<void>`).
 * @warning `value()`는 에러 상태에서 호출하면 `std::bad_variant_access`를 던집니다.
 *          반드시 `has_value()` 또는 `operator bool()`로 상태를 확인한 후 호출하세요.
 */
template <typename T>
class Result {
public:
  /**
   * @brief 성공값으로 Result를 구성합니다 (암묵적 변환 허용).
   * @param v 저장할 성공값. 이동(move)됩니다.
   */
  Result(T v) : data_(std::move(v)) {}                          // NOLINT

  /**
   * @brief 명시적 성공 Result를 생성하는 팩토리 메서드.
   * @param v 저장할 성공값. 이동(move)됩니다.
   * @returns 성공 상태의 Result<T>.
   */
  static Result ok(T v) { return Result(std::move(v)); }

  /**
   * @brief `std::error_code`로부터 에러 Result를 직접 생성합니다.
   * @param ec 에러 코드.
   * @returns 에러 상태의 Result<T>.
   */
  static Result err(std::error_code ec) {
    Result r;
    r.data_ = ec;
    return r;
  }

  /**
   * @brief `unexpected<E>` 태그로부터 에러 Result를 구성합니다 (암묵적 변환 허용).
   *
   * 이 생성자를 통해 `return unexpected(ec);` 패턴이 동작합니다.
   * `unexpected`의 값은 `std::error_code`로 변환됩니다.
   *
   * @tparam E `std::error_code`로 변환 가능한 에러 타입.
   * @param u 에러를 담고 있는 unexpected 태그 객체.
   */
  template <typename E>
  Result(unexpected<E> u)                                       // NOLINT
      : data_(std::in_place_index<2>, std::error_code(u.value)) {}

  /**
   * @brief 성공 상태인지 확인합니다.
   * @returns 성공값을 보유하면 true, 에러 상태면 false.
   */
  bool has_value() const noexcept {
    return std::holds_alternative<T>(data_);
  }

  /**
   * @brief 성공 상태인지 확인하는 bool 변환 연산자.
   * @returns `has_value()`와 동일.
   */
  explicit operator bool() const noexcept { return has_value(); }

  /**
   * @brief 성공값에 대한 lvalue 참조를 반환합니다.
   * @returns 저장된 값의 참조.
   * @throws std::bad_variant_access 에러 상태에서 호출 시.
   */
  T &value() & { return std::get<T>(data_); }

  /** @brief 성공값에 대한 const lvalue 참조를 반환합니다. @throws std::bad_variant_access */
  const T &value() const & { return std::get<T>(data_); }

  /** @brief 성공값의 rvalue 참조를 반환합니다 (이동 시 사용). @throws std::bad_variant_access */
  T &&value() && { return std::get<T>(std::move(data_)); }

  /**
   * @brief 에러 코드를 반환합니다.
   * @returns 에러 상태면 에러 코드, 성공 상태면 기본 구성된 (무효) error_code.
   */
  std::error_code error() const noexcept {
    if (auto *ec = std::get_if<std::error_code>(&data_))
      return *ec;
    return {};
  }

  /** @brief `value()`의 단축 연산자. @throws std::bad_variant_access */
  T &operator*() & { return value(); }

  /** @brief `value()`의 const 단축 연산자. @throws std::bad_variant_access */
  const T &operator*() const & { return value(); }

  /** @brief 성공값의 멤버에 직접 접근하는 포인터 연산자. @throws std::bad_variant_access */
  T *operator->() { return &value(); }

  /** @brief 성공값의 멤버에 직접 접근하는 const 포인터 연산자. @throws std::bad_variant_access */
  const T *operator->() const { return &value(); }

private:
  /** @brief 내부 팩토리 메서드(`err`)에서만 사용되는 기본 생성자. */
  Result() = default;

  /**
   * @brief 내부 저장소.
   *
   * - index 0 (monostate): 초기화 전 또는 `err()` 빌더 내부 임시 상태.
   * - index 1 (T): 성공 상태.
   * - index 2 (std::error_code): 에러 상태.
   */
  std::variant<std::monostate, T, std::error_code> data_;
};

// ─── Result<void> ────────────────────────────────────────────────────────────

/**
 * @brief 반환값 없이 성공/실패만 표현하는 `Result<void>` 특수화.
 *
 * I/O 작업처럼 완료 여부만 중요하고 반환값이 없는 함수에 적합합니다.
 *
 * @code
 * Result<void> flush(int fd) {
 *     if (fsync(fd) != 0)
 *         return unexpected(std::error_code(errno, std::system_category()));
 *     return Result<void>::ok();
 * }
 *
 * auto r = flush(fd);
 * if (!r) {
 *     std::cerr << "flush 실패: " << r.error().message() << '\n';
 * }
 * @endcode
 *
 * @note `value()` 멤버는 없습니다 — 성공 여부만 `has_value()` 또는 `operator bool()`로 확인합니다.
 */
template <>
class Result<void> {
public:
  /**
   * @brief 기본 생성자: 성공 상태로 초기화합니다.
   */
  Result() : ok_(true) {}

  /**
   * @brief 성공 Result<void>를 생성하는 팩토리 메서드.
   * @returns 성공 상태의 Result<void>.
   */
  static Result ok() { return Result{}; }

  /**
   * @brief 에러 코드로 에러 Result<void>를 생성합니다.
   * @param ec 에러 코드.
   * @returns 에러 상태의 Result<void>.
   */
  static Result err(std::error_code ec) {
    Result r;
    r.ok_ = false;
    r.ec_ = ec;
    return r;
  }

  /**
   * @brief `unexpected<E>` 태그로부터 에러 Result<void>를 구성합니다 (암묵적 변환 허용).
   * @tparam E `std::error_code`로 변환 가능한 에러 타입.
   * @param u 에러를 담고 있는 unexpected 태그 객체.
   */
  template <typename E>
  Result(unexpected<E> u) : ok_(false), ec_(u.value) {}        // NOLINT

  /**
   * @brief 성공 상태인지 확인합니다.
   * @returns 성공이면 true, 에러면 false.
   */
  bool has_value() const noexcept { return ok_; }

  /**
   * @brief 성공 상태인지 확인하는 bool 변환 연산자.
   * @returns `has_value()`와 동일.
   */
  explicit operator bool() const noexcept { return ok_; }

  /**
   * @brief 에러 코드를 반환합니다.
   * @returns 에러 상태면 에러 코드, 성공 상태면 기본 구성된 (무효) error_code.
   */
  std::error_code error() const noexcept { return ec_; }

private:
  /** @brief 성공 여부 플래그. */
  bool ok_ = false;

  /** @brief 에러 상태일 때의 에러 코드. 성공 상태에서는 기본값(무효)입니다. */
  std::error_code ec_;
};

// ─── Buffer Views ─────────────────────────────────────────────────────────────

/**
 * @brief 읽기 전용 바이트 버퍼 뷰 (zero-copy).
 *
 * `std::span<const uint8_t>`의 별칭입니다.
 * 데이터를 복사하지 않고 기존 버퍼를 참조할 때 사용합니다.
 * 파싱, 직렬화, I/O 등 핫 패스에서 불필요한 복사를 방지합니다.
 *
 * @warning 뷰가 참조하는 버퍼의 수명이 뷰보다 길어야 합니다.
 */
using BufferView = std::span<const uint8_t>;

/**
 * @brief 쓰기 가능한 바이트 버퍼 뷰 (zero-copy).
 *
 * `std::span<uint8_t>`의 별칭입니다.
 * 수신 버퍼 등 직접 쓰기가 필요한 경우에 사용합니다.
 *
 * @warning 뷰가 참조하는 버퍼의 수명이 뷰보다 길어야 합니다.
 */
using MutableBufferView = std::span<uint8_t>;

} // namespace draco

/** @} */ // end of qbuem_common
