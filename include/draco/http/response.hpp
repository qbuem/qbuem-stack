#pragma once

#include <draco/common.hpp>

#include <ctime>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace draco {

/** @brief Options for the Set-Cookie response header. */
struct CookieOptions {
  std::string path       = "/";
  std::string domain;           // empty → omit Domain attribute
  std::string same_site  = "";  // "Strict" | "Lax" | "None" | "" (omit)
  int         max_age    = -1;  // < 0 → omit Max-Age (session cookie)
  bool        http_only  = false;
  bool        secure     = false;
};

/**
 * @brief HTTP Response builder.
 *
 * Use body() with a pre-serialized string for structured data (JSON, etc.).
 * There is intentionally no built-in JSON method — serialize with whichever
 * library you choose and pass the result to body().
 *
 * Cookie example:
 *   res.set_cookie("session", "abc123",
 *                  CookieOptions{.http_only=true, .same_site="Lax"});
 */
class Response {
public:
  Response() = default;

  Response &status(int code);
  Response &header(std::string_view key, std::string_view value);
  Response &body(std::string_view b);

  /**
   * @brief Append a Set-Cookie response header.
   *
   * Multiple cookies are supported; each call appends one Set-Cookie line.
   */
  Response &set_cookie(std::string_view name, std::string_view value,
                       CookieOptions opts = CookieOptions{});

  /**
   * @brief Set the ETag response header.
   *
   * The value is wrapped in double-quotes to form a strong entity-tag per
   * RFC 7232.  Pass an already-quoted string to use a weak ETag (W/"...").
   *
   * Example:
   *   res.etag("abc123");         // ETag: "abc123"
   *   res.etag("W/\"v2\"");       // ETag: W/"v2"  (weak, caller quotes)
   */
  Response &etag(std::string_view value) {
    // If the caller already quoted the value, use as-is; otherwise wrap it.
    if (!value.empty() && value.front() == '"')
      return header("ETag", value);
    if (value.size() >= 2 && value[0] == 'W' && value[1] == '/')
      return header("ETag", value);
    return header("ETag", "\"" + std::string(value) + "\"");
  }

  /**
   * @brief Set the Last-Modified response header from a UTC time_t.
   *
   * Formats the timestamp as an HTTP-date (RFC 7231 §7.1.1.1).
   */
  Response &last_modified(std::time_t t) {
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[48];
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return header("Last-Modified", buf);
  }

  /** @brief Return the value of a response header (empty string_view if not set). */
  std::string_view get_header(std::string_view key) const {
    auto it = headers_.find(std::string(key));
    return (it != headers_.end()) ? std::string_view(it->second) : std::string_view{};
  }

  /** @brief Return the response body. */
  std::string_view get_body() const { return body_; }

  /** @brief Return the HTTP status code. */
  int status_code() const { return status_code_; }

  std::string serialize() const;

private:
  std::string_view status_to_string(int code) const;

  int         status_code_ = 200;
  std::unordered_map<std::string, std::string> headers_;
  std::vector<std::string> cookies_; // raw Set-Cookie values
  std::string body_;
};

} // namespace draco
