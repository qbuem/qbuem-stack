#pragma once

#include <draco/common.hpp>

#include <string>
#include <string_view>
#include <unordered_map>

namespace draco {

enum class Method { Get, Post, Put, Delete, Patch, Options, Head, Unknown };

inline Method string_to_method(std::string_view m) {
  if (m == "GET")     return Method::Get;
  if (m == "POST")    return Method::Post;
  if (m == "PUT")     return Method::Put;
  if (m == "DELETE")  return Method::Delete;
  if (m == "PATCH")   return Method::Patch;
  if (m == "OPTIONS") return Method::Options;
  if (m == "HEAD")    return Method::Head;
  return Method::Unknown;
}

/**
 * @brief HTTP Request.
 *
 * Body is exposed as raw bytes via body(). JSON parsing is the application's
 * responsibility — use your preferred library (simdjson, nlohmann/json, glaze,
 * …) directly on body().
 *
 * Query string parameters are available via query(key).
 * The path() accessor returns only the path component (before '?').
 */
class Request {
public:
  Request() = default;

  Method          method()       const { return method_; }
  std::string_view path()        const { return path_; }
  std::string_view body()        const { return body_; }
  std::string_view query_string() const { return query_string_; }

  std::string_view header(std::string_view key) const {
    auto it = headers_.find(std::string(key));
    return (it != headers_.end()) ? it->second : std::string_view{};
  }

  std::string_view param(std::string_view key) const {
    auto it = params_.find(std::string(key));
    return (it != params_.end()) ? it->second : std::string_view{};
  }

  /**
   * @brief Return the value of a URL query parameter.
   *
   * Scans the raw query string (no allocation).
   * Returns empty string_view when the key is absent.
   * Values are returned as-is (percent-encoded); callers that need decoded
   * values must decode themselves.
   */
  std::string_view query(std::string_view key) const {
    std::string_view qs = query_string_;
    while (!qs.empty()) {
      size_t amp  = qs.find('&');
      std::string_view pair = (amp == std::string_view::npos) ? qs
                                                               : qs.substr(0, amp);
      size_t eq = pair.find('=');
      if (eq != std::string_view::npos && pair.substr(0, eq) == key)
        return pair.substr(eq + 1);

      if (amp == std::string_view::npos) break;
      qs = qs.substr(amp + 1);
    }
    return {};
  }

  // Setters used by the parser / router
  void set_method(Method m) { method_ = m; }

  /**
   * Split the raw request-target at '?' and store path and query string
   * separately.  Routing uses only the path component.
   */
  void set_path(std::string_view raw) {
    size_t q = raw.find('?');
    if (q == std::string_view::npos) {
      path_         = std::string(raw);
      query_string_ = {};
    } else {
      path_         = std::string(raw.substr(0, q));
      query_string_ = std::string(raw.substr(q + 1));
    }
  }

  void set_body(std::string_view b) { body_ = std::string(b); }

  void add_header(std::string_view key, std::string_view value) {
    headers_[std::string(key)] = std::string(value);
  }
  void set_param(std::string_view key, std::string_view value) {
    params_[std::string(key)] = std::string(value);
  }

private:
  Method      method_       = Method::Unknown;
  std::string path_;
  std::string query_string_;
  std::string body_;
  std::unordered_map<std::string, std::string> headers_;
  std::unordered_map<std::string, std::string> params_;
};

} // namespace draco
