#pragma once

/**
 * @file qbuem/url.hpp
 * @brief URL utility functions: percent-encoding / decoding.
 *
 * Header-only, zero external dependencies.
 */

#include <cctype>
#include <string>
#include <string_view>

namespace qbuem {

namespace detail {

inline int hex_digit(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

} // namespace detail

/**
 * Decode a percent-encoded URL component.
 *
 * Converts `%XX` sequences to the corresponding bytes and `+` to space
 * (application/x-www-form-urlencoded convention).
 *
 * Invalid `%XX` sequences (non-hex digits) are left unchanged.
 *
 * Example:
 *   url_decode("hello%20world%21") → "hello world!"
 *   url_decode("q=foo+bar")         → "q=foo bar"
 *
 * @param encoded  Percent-encoded string_view.
 * @returns        Decoded std::string.
 */
[[nodiscard]] inline std::string url_decode(std::string_view encoded) {
  std::string out;
  out.reserve(encoded.size()); // decoded length ≤ encoded length

  for (size_t i = 0; i < encoded.size(); ++i) {
    char c = encoded[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < encoded.size()) {
      int hi = detail::hex_digit(encoded[i + 1]);
      int lo = detail::hex_digit(encoded[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += static_cast<char>((hi << 4) | lo);
        i += 2;
      } else {
        out += c; // invalid sequence — pass through
      }
    } else {
      out += c;
    }
  }
  return out;
}

/**
 * Percent-encode a string for use in a URL query component.
 *
 * Encodes all characters except unreserved characters
 * (A-Z a-z 0-9 - _ . ~) as per RFC 3986 §2.3.
 *
 * Example:
 *   url_encode("hello world!") → "hello%20world%21"
 *
 * @param raw  Raw string to encode.
 * @returns    Percent-encoded std::string.
 */
[[nodiscard]] inline std::string url_encode(std::string_view raw) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(raw.size() * 3); // worst-case: every char is encoded

  for (unsigned char c : raw) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[(c >> 4) & 0x0F];
      out += kHex[c & 0x0F];
    }
  }
  return out;
}

} // namespace qbuem
