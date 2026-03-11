#pragma once

#include <draco/common.hpp>

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

  std::string serialize() const;

private:
  std::string_view status_to_string(int code) const;

  int         status_code_ = 200;
  std::unordered_map<std::string, std::string> headers_;
  std::vector<std::string> cookies_; // raw Set-Cookie values
  std::string body_;
};

} // namespace draco
