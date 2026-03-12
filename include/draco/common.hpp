#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

namespace draco {

// ─── unexpected<E> ───────────────────────────────────────────────────────────
// Error-tag type, mirrors std::unexpected from C++23.
// Usage:  return unexpected(std::make_error_code(std::errc::io_error));
template <typename E>
struct unexpected {
  E value;
  explicit unexpected(E e) : value(std::move(e)) {}
};
// Deduction guide
template <typename E> unexpected(E) -> unexpected<E>;

// ─── Result<T> ───────────────────────────────────────────────────────────────
// Minimal std::expected<T, std::error_code>-compatible type for C++20.
// Provides .has_value(), .value(), .error(), and operator bool().
template <typename T>
class Result {
public:
  // Success constructor
  Result(T v) : data_(std::move(v)) {}                          // NOLINT
  static Result ok(T v) { return Result(std::move(v)); }

  // Error constructor — direct
  static Result err(std::error_code ec) {
    Result r;
    r.data_ = ec;
    return r;
  }

  // Error constructor — from unexpected<E> tag (mirrors std::expected idiom)
  template <typename E>
  Result(unexpected<E> u)                                       // NOLINT
      : data_(std::in_place_index<2>, std::error_code(u.value)) {}

  bool has_value() const noexcept {
    return std::holds_alternative<T>(data_);
  }
  explicit operator bool() const noexcept { return has_value(); }

  T &value() & { return std::get<T>(data_); }
  const T &value() const & { return std::get<T>(data_); }
  T &&value() && { return std::get<T>(std::move(data_)); }

  std::error_code error() const noexcept {
    if (auto *ec = std::get_if<std::error_code>(&data_))
      return *ec;
    return {};
  }

  T &operator*() & { return value(); }
  const T &operator*() const & { return value(); }
  T *operator->() { return &value(); }
  const T *operator->() const { return &value(); }

private:
  Result() = default;
  std::variant<std::monostate, T, std::error_code> data_;
};

// Specialisation for Result<void>
template <>
class Result<void> {
public:
  Result() : ok_(true) {}
  static Result ok() { return Result{}; }
  static Result err(std::error_code ec) {
    Result r;
    r.ok_ = false;
    r.ec_ = ec;
    return r;
  }

  // From unexpected<E> tag
  template <typename E>
  Result(unexpected<E> u) : ok_(false), ec_(u.value) {}        // NOLINT

  bool has_value() const noexcept { return ok_; }
  explicit operator bool() const noexcept { return ok_; }
  std::error_code error() const noexcept { return ec_; }

private:
  bool ok_ = false;
  std::error_code ec_;
};

// Use std::span for zero-copy buffer views
using BufferView = std::span<const uint8_t>;
using MutableBufferView = std::span<uint8_t>;

} // namespace draco
