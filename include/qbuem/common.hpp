#pragma once

/**
 * @file qbuem/common.hpp
 * @brief Global common type definitions for qbuem-stack.
 * @ingroup qbuem_common
 *
 * This header defines the core types used throughout the library:
 *
 * - `unexpected<E>` : Alias for `std::unexpected<E>` (C++23 <expected>)
 * - `Result<T>`     : Sum type holding a success value or `std::error_code`
 * - `Result<void>`  : Specialisation expressing success/failure without a return value
 * - `BufferView`    : Read-only byte view (zero-copy, std::span-based)
 * - `MutableBufferView` : Writable byte view
 *
 * ### C++23
 * qbuem-stack targets C++23. `std::unexpected<E>` from `<expected>` is used directly.
 * `Result<T>` wraps `std::error_code` as the error type and provides monadic
 * operations compatible with `std::expected` conventions.
 */

/**
 * @defgroup qbuem_common Common Types
 * @brief Common types and utilities used throughout the library.
 *
 * All public APIs propagate errors through the `Result<T>` type defined here.
 * No exceptions are thrown, eliminating exception overhead on hot paths.
 * @{
 */

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <variant>
#include <vector>

namespace qbuem {

// ─── unexpected<E> ───────────────────────────────────────────────────────────

/**
 * @brief Alias for C++23 `std::unexpected<E>`.
 *
 * Used to unambiguously construct the error path of a `Result<T>`.
 *
 * @code
 * Result<int> read_byte(int fd) {
 *     if (fd < 0)
 *         return unexpected(std::make_error_code(std::errc::bad_file_descriptor));
 *     // ...
 * }
 * @endcode
 *
 * @tparam E Error value type. Usually `std::error_code`.
 */
template <typename E>
using unexpected = std::unexpected<E>;

// ─── Result<T> ───────────────────────────────────────────────────────────────

/**
 * @brief Sum type holding either a success value `T` or a `std::error_code`.
 *
 * Equivalent to `std::expected<T, std::error_code>` with additional monadic
 * helpers. Supports value-based error propagation without exceptions.
 *
 * ### States
 * - Success: holds a `T` value. `has_value()` == true, `operator bool()` == true.
 * - Error:   holds a `std::error_code`. `has_value()` == false.
 * - Initial (private default ctor): monostate — used only inside factory methods.
 *
 * ### Monadic operations
 * - `map(f)`            — Functor: transform success value, propagate error unchanged.
 * - `and_then(f)`       — Monad flatMap: f must return `Result<U>`.
 * - `transform_error(f)`— Map the error code; success passes through unchanged.
 * - `value_or(default)` — Return value or a fallback on error.
 *
 * @code
 * Result<int> parse_port(std::string_view s) {
 *     int port = 0;
 *     // ... parsing ...
 *     if (port < 0 || port > 65535)
 *         return unexpected(std::make_error_code(std::errc::invalid_argument));
 *     return port;
 * }
 *
 * auto r = parse_port("8080");
 * if (!r) {
 *     std::cerr << "error: " << r.error().message() << '\n';
 *     return;
 * }
 * std::cout << "port: " << *r << '\n';
 * @endcode
 *
 * @tparam T Type of the success value.
 *
 * @note See also the `Result<void>` specialisation.
 * @warning `value()` throws `std::bad_variant_access` when called on an error result.
 *          Always check `has_value()` or `operator bool()` first.
 */
template <typename T>
class Result {
public:
  /**
   * @brief Construct a success Result from a value (implicit conversion).
   * @param v Success value (moved).
   */
  Result(T v) : data_(std::move(v)) {}                          // NOLINT

  /**
   * @brief Factory method for an explicit success Result.
   * @param v Success value (moved).
   * @returns Result<T> in the success state.
   */
  static Result ok(T v) { return Result(std::move(v)); }

  /**
   * @brief Construct an error Result directly from a `std::error_code`.
   * @param ec Error code.
   * @returns Result<T> in the error state.
   */
  static Result err(std::error_code ec) {
    Result r;
    r.data_ = ec;
    return r;
  }

  /**
   * @brief Construct an error Result from an `unexpected<E>` tag (implicit conversion).
   *
   * Enables the `return unexpected(ec);` pattern.
   * The wrapped value is converted to `std::error_code`.
   *
   * @tparam E Type convertible to `std::error_code`.
   * @param u  The unexpected tag object.
   */
  template <typename E>
  Result(unexpected<E> u) : data_(std::in_place_index<2>) {      // NOLINT
    if constexpr (std::is_same_v<E, std::error_code>) {
      std::get<2>(data_) = u.error();
    } else {
      std::get<2>(data_) = std::make_error_code(u.error());
    }
  }

  /**
   * @brief Check whether the result holds a success value.
   * @returns true if in the success state, false on error.
   */
  bool has_value() const noexcept {
    return std::holds_alternative<T>(data_);
  }

  /**
   * @brief Boolean conversion — equivalent to `has_value()`.
   */
  explicit operator bool() const noexcept { return has_value(); }

  /**
   * @brief Return lvalue reference to the success value.
   * @throws std::bad_variant_access when called on an error result.
   */
  T &value() & { return std::get<T>(data_); }

  /** @brief Return const lvalue reference to the success value. @throws std::bad_variant_access */
  const T &value() const & { return std::get<T>(data_); }

  /** @brief Return rvalue reference to the success value (for moves). @throws std::bad_variant_access */
  T &&value() && { return std::get<T>(std::move(data_)); }

  /**
   * @brief Return the error code.
   * @returns The error code on failure; a default-constructed (invalid) error_code on success.
   */
  std::error_code error() const noexcept {
    if (auto *ec = std::get_if<std::error_code>(&data_))
      return *ec;
    return {};
  }

  /** @brief Dereference operator — shorthand for `value()`. @throws std::bad_variant_access */
  T &operator*() & { return value(); }

  /** @brief Const dereference operator. @throws std::bad_variant_access */
  const T &operator*() const & { return value(); }

  /** @brief Arrow operator for member access on the success value. @throws std::bad_variant_access */
  T *operator->() { return &value(); }

  /** @brief Const arrow operator. @throws std::bad_variant_access */
  const T *operator->() const { return &value(); }

  // ─── Monadic operations ──────────────────────────────────────────────────

  /**
   * @brief Transform the success value (Functor map).
   *
   * Calls `f(value)` on success and wraps the result in `Result<U>`.
   * Propagates the error unchanged if in the error state.
   *
   * @code
   * Result<int> r = parse_int("42");
   * Result<std::string> s = r.map([](int n){ return std::to_string(n); });
   * @endcode
   */
  template <typename F>
  auto map(F &&f) const & -> Result<std::invoke_result_t<F, const T &>> {
    using U = std::invoke_result_t<F, const T &>;
    if (has_value()) return Result<U>(std::forward<F>(f)(value()));
    return Result<U>::err(error());
  }

  template <typename F>
  auto map(F &&f) && -> Result<std::invoke_result_t<F, T &&>> {
    using U = std::invoke_result_t<F, T &&>;
    if (has_value()) return Result<U>(std::forward<F>(f)(std::move(value())));
    return Result<U>::err(error());
  }

  /**
   * @brief Monadic flatMap — chain operations that may fail (Monad bind).
   *
   * Calls `f(value)` on success; `f` must return `Result<U>`.
   * Propagates the error unchanged if in the error state.
   *
   * @code
   * Result<std::string> r = fetch_url(url);
   * Result<ParsedData> d = r.and_then([](const std::string& body) {
   *     return parse_json(body);  // returns Result<ParsedData>
   * });
   * @endcode
   */
  template <typename F>
  auto and_then(F &&f) const & -> std::invoke_result_t<F, const T &> {
    using Ret = std::invoke_result_t<F, const T &>;
    if (has_value()) return std::forward<F>(f)(value());
    return Ret::err(error());
  }

  template <typename F>
  auto and_then(F &&f) && -> std::invoke_result_t<F, T &&> {
    using Ret = std::invoke_result_t<F, T &&>;
    if (has_value()) return std::forward<F>(f)(std::move(value()));
    return Ret::err(error());
  }

  /**
   * @brief Transform the error code (error functor).
   *
   * Calls `f(error())` on failure and returns a new `Result<T>` with the
   * transformed error. Passes through unchanged if in the success state.
   *
   * @code
   * auto r = do_io().transform_error([](std::error_code) {
   *     return std::make_error_code(std::errc::io_error);
   * });
   * @endcode
   */
  template <typename F>
  Result<T> transform_error(F &&f) const & {
    if (!has_value()) return Result<T>::err(std::forward<F>(f)(error()));
    return *this;
  }

  /**
   * @brief Return the success value or a fallback on error.
   *
   * @code
   * int n = parse_int(s).value_or(0);
   * @endcode
   */
  T value_or(T default_value) const & {
    return has_value() ? value() : std::move(default_value);
  }

  T value_or(T default_value) && {
    return has_value() ? std::move(value()) : std::move(default_value);
  }

private:
  /** @brief Default constructor — used only by the `err()` factory method. */
  Result() = default;

  /**
   * @brief Internal storage.
   *
   * - index 0 (monostate): uninitialized / temporary state inside `err()`.
   * - index 1 (T):              success state.
   * - index 2 (std::error_code): error state.
   */
  std::variant<std::monostate, T, std::error_code> data_;
};

// ─── Result<void> ────────────────────────────────────────────────────────────

/**
 * @brief `Result<void>` specialisation — success/failure without a return value.
 *
 * Suitable for I/O operations where only completion status matters.
 *
 * @code
 * Result<void> flush(int fd) {
 *     if (fsync(fd) != 0)
 *         return unexpected(std::error_code(errno, std::system_category()));
 *     return Result<void>::ok();
 * }
 *
 * auto r = flush(fd);
 * if (!r) { std::cerr << "flush failed: " << r.error().message() << '\n'; }
 * @endcode
 *
 * @note There is no `value()` member — check `has_value()` or `operator bool()`.
 */
template <>
class Result<void> {
public:
  /** @brief Default constructor: initialises to the success state. */
  Result() : ok_(true) {}

  /**
   * @brief Factory method for an explicit success `Result<void>`.
   * @returns Result<void> in the success state.
   */
  static Result ok() { return Result{}; }

  /**
   * @brief Construct an error `Result<void>` from a `std::error_code`.
   * @param ec Error code.
   * @returns Result<void> in the error state.
   */
  static Result err(std::error_code ec) {
    Result r;
    r.ok_ = false;
    r.ec_ = ec;
    return r;
  }

  /**
   * @brief Construct an error `Result<void>` from an `unexpected<E>` tag (implicit).
   * @tparam E Type convertible to `std::error_code`.
   * @param u  The unexpected tag object.
   */
  template <typename E>
  Result(unexpected<E> u) : ok_(false) {                         // NOLINT
    if constexpr (std::is_same_v<E, std::error_code>) {
      ec_ = u.error();
    } else {
      ec_ = std::make_error_code(u.error());
    }
  }

  /** @brief Returns true if in the success state. */
  bool has_value() const noexcept { return ok_; }

  /** @brief Boolean conversion — equivalent to `has_value()`. */
  explicit operator bool() const noexcept { return ok_; }

  /**
   * @brief Return the error code.
   * @returns Error code on failure; default-constructed (invalid) code on success.
   */
  std::error_code error() const noexcept { return ec_; }

private:
  /** @brief Success flag. */
  bool ok_ = false;

  /** @brief Error code when in the error state. Default-constructed (invalid) on success. */
  std::error_code ec_;
};

// ─── Buffer Views ─────────────────────────────────────────────────────────────

/**
 * @brief Read-only byte buffer view (zero-copy).
 *
 * Alias for `std::span<const uint8_t>`.
 * Used in parsing, serialisation, and I/O hot paths to avoid unnecessary copies.
 *
 * @warning The referenced buffer must outlive the view.
 */
using BufferView = std::span<const uint8_t>;

/**
 * @brief Writable byte buffer view (zero-copy).
 *
 * Alias for `std::span<uint8_t>`.
 * Used for receive buffers and other write-capable byte spans.
 *
 * @warning The referenced buffer must outlive the view.
 */
using MutableBufferView = std::span<uint8_t>;

} // namespace qbuem

/** @} */ // end of qbuem_common
