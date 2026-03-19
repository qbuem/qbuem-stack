#pragma once

/**
 * @file qbuem/common.hpp
 * @brief Global common type definitions for qbuem-stack.
 * @ingroup qbuem_common
 *
 * This header defines the core types used throughout the library:
 *
 * - `Result<T>`         : Alias for `std::expected<T, std::error_code>` (C++23)
 * - `Result<void>`      : Alias for `std::expected<void, std::error_code>` (C++23)
 * - `unexpected<E>`     : Alias for `std::unexpected<E>` (C++23)
 * - `BufferView`        : Read-only byte view (zero-copy, std::span-based)
 * - `MutableBufferView` : Writable byte view
 *
 * ### C++23
 * qbuem-stack targets C++23. `std::expected<T, E>` and `std::unexpected<E>` from
 * `<expected>` are used directly. The `Result<T>` alias provides a concise name
 * that reads as "this function either succeeds with T or fails with std::error_code".
 *
 * ### Monadic operations (std::expected C++23 API)
 * - `transform(f)`         — maps success value, propagates error (was: map)
 * - `and_then(f)`          — flatMap: f must return Result<U>
 * - `transform_error(f)`   — maps error, propagates success
 * - `or_else(f)`           — on error, call f(error) returning Result<T>
 * - `value_or(default)`    — success value or fallback
 *
 * ### Factory patterns
 * ```cpp
 * Result<int>   parse_port(std::string_view s);
 *
 * // success: implicit conversion from T
 * return 8080;
 *
 * // error: std::unexpected wrapping std::error_code
 * return std::unexpected(std::make_error_code(std::errc::invalid_argument));
 *
 * // void success: value-initialised expected (default = has_value())
 * Result<void> flush(int fd) {
 *     if (::fsync(fd) != 0)
 *         return std::unexpected(std::error_code{errno, std::system_category()});
 *     return {};   // <-- success
 * }
 * ```
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
#include <span>
#include <string_view>
#include <system_error>

namespace qbuem {

// ─── unexpected<E> ────────────────────────────────────────────────────────────

/**
 * @brief Alias for C++23 `std::unexpected<E>`.
 *
 * Used to unambiguously construct the error path of a `Result<T>`:
 * @code
 * return unexpected(std::make_error_code(std::errc::invalid_argument));
 * @endcode
 * @tparam E Error value type. Usually `std::error_code`.
 */
template <typename E>
using unexpected = std::unexpected<E>;

// ─── Result<T> ────────────────────────────────────────────────────────────────

/**
 * @brief Alias for `std::expected<T, std::error_code>` (C++23).
 *
 * Represents a value of type `T` on success, or a `std::error_code` on failure.
 * Use `[[nodiscard]]` on all functions returning `Result<T>`.
 *
 * ### Common patterns
 * ```cpp
 * // Success — implicit conversion from T:
 * Result<int> count() { return 42; }
 *
 * // Error — std::unexpected:
 * Result<int> parse(std::string_view s) {
 *     return std::unexpected(std::make_error_code(std::errc::invalid_argument));
 * }
 *
 * // Void success — default-constructed expected (has_value() == true):
 * Result<void> init() { return {}; }
 *
 * // Monadic chaining:
 * auto r = fetch(url)
 *     .transform([](const Response& r){ return r.status(); })
 *     .and_then([](int code) -> Result<std::string> {
 *         if (code != 200) return std::unexpected(std::make_error_code(std::errc::protocol_error));
 *         return "ok";
 *     })
 *     .value_or("unknown");
 * ```
 *
 * @tparam T Success value type. Use `void` for operations that only signal success/failure.
 */
template <typename T>
using Result = std::expected<T, std::error_code>;

// ─── Buffer Views ──────────────────────────────────────────────────────────────

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
