#pragma once

/**
 * @file qbuem/compat/print.hpp
 * @brief C++23 <print> compatibility shim for compilers that lack std::print/std::println.
 *
 * GCC 13 / Clang 17 implement most of C++23 but do not ship <print> yet.
 * GCC 14+ and Clang 18+ include the full <print> header.
 *
 * This header provides `std::print` and `std::println` as inline wrappers
 * over `std::format` + `std::fwrite` when the standard implementation is absent.
 *
 * Usage: replace `#include <print>` with `#include <qbuem/compat/print.hpp>`.
 * When the compiler gains native <print>, this header becomes a no-op pass-through.
 */

#if __has_include(<print>)
#  include <print>
#else
// Shim: provides std::print / std::println using std::format.
#  include <cstdio>
#  include <format>
#  include <string_view>

namespace std {  // NOLINT(cert-dcl58-cpp) — intentionally extending std for C++23 polyfill

template <typename... Args>
inline void print(std::format_string<Args...> fmt, Args&&... args) {
    auto s = std::format(fmt, std::forward<Args>(args)...);
    std::fwrite(s.data(), 1, s.size(), stdout);
}

template <typename... Args>
inline void println(std::format_string<Args...> fmt, Args&&... args) {
    auto s = std::format(fmt, std::forward<Args>(args)...);
    s += '\n';
    std::fwrite(s.data(), 1, s.size(), stdout);
}

inline void println() {
    std::fputc('\n', stdout);
}

inline void print(std::FILE* f, std::string_view s) {
    std::fwrite(s.data(), 1, s.size(), f);
}

inline void println(std::FILE* f, std::string_view s) {
    std::fwrite(s.data(), 1, s.size(), f);
    std::fputc('\n', f);
}

template <typename... Args>
inline void print(std::FILE* f, std::format_string<Args...> fmt, Args&&... args) {
    auto s = std::format(fmt, std::forward<Args>(args)...);
    std::fwrite(s.data(), 1, s.size(), f);
}

template <typename... Args>
inline void println(std::FILE* f, std::format_string<Args...> fmt, Args&&... args) {
    auto s = std::format(fmt, std::forward<Args>(args)...);
    s += '\n';
    std::fwrite(s.data(), 1, s.size(), f);
}

} // namespace std

#endif // __has_include(<print>)
