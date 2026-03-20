#pragma once

/**
 * @file qbuem/http/request.hpp
 * @brief HTTP request value type — method, path, header, and body accessors.
 * @defgroup qbuem_http_request HTTP Request
 * @ingroup qbuem_http
 *
 * Request is an immutable value type. body() returns raw bytes;
 * JSON parsing is the application's responsibility.
 * Convenience accessors: query(), cookie(), form(), param().
 * path() returns only the part before '?'.
 * @{
 */

#include <qbuem/common.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace qbuem {

// ---------------------------------------------------------------------------
// Heterogeneous hash/equality for string maps — allows string_view lookup
// without constructing a std::string key.
// Used by the lazy query-parameter cache only (cold path).
// ---------------------------------------------------------------------------
struct StringViewHash {
  using is_transparent = void;
  std::size_t operator()(std::string_view sv) const noexcept {
    return std::hash<std::string_view>{}(sv);
  }
};
struct StringViewEqual {
  using is_transparent = void;
  bool operator()(std::string_view a, std::string_view b) const noexcept {
    return a == b;
  }
};
using StringMap = std::unordered_map<std::string, std::string,
                                     StringViewHash, StringViewEqual>;

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
 * Convenience accessors:
 *   query(key)  — URL query parameter (?key=val)
 *   cookie(key) — Cookie header field (Cookie: key=val; ...)
 *   form(key)   — application/x-www-form-urlencoded body field
 *   param(key)  — URL path parameter (:name segments)
 *
 * The path() accessor returns only the path component (before '?').
 */
class Request {
public:
  Request() = default;

  Method           method()        const { return method_; }
  std::string_view path()          const { return path_; }
  std::string_view body()          const { return body_; }
  std::string_view query_string()  const { return query_string_; }

  /**
   * Return the client IP address as seen by the server's accept() call.
   *
   * This is the immediate peer address (may be a load-balancer or proxy IP).
   * For the originating client IP use req.header("X-Forwarded-For") or
   * req.header("X-Real-IP") when behind a trusted reverse proxy.
   */
  std::string_view remote_addr()   const { return remote_addr_; }

  std::string_view header(std::string_view key) const noexcept {
    // Linear scan over flat inline array — O(n), cache-friendly, zero allocation.
    for (uint8_t i = 0; i < header_count_; ++i)
      if (headers_flat_[i].first == key)
        return headers_flat_[i].second;
    return {};
  }

  std::string_view param(std::string_view key) const noexcept {
    for (uint8_t i = 0; i < param_count_; ++i)
      if (params_flat_[i].first == key)
        return params_flat_[i].second;
    return {};
  }

  /**
   * Return the value of a URL query parameter (?key=val&...).
   *
   * The query string is parsed lazily on the first call and cached.
   * Subsequent calls are O(1) hash-map lookups with no allocation.
   * Values are returned percent-encoded; decode if needed.
   */
  std::string_view query(std::string_view key) const {
    ensure_query_cache();
    auto it = query_cache_->find(key);
    return (it != query_cache_->end()) ? it->second : std::string_view{};
  }

  /**
   * Return the value of a named cookie from the Cookie request header.
   * Format: "name=val; name2=val2"
   */
  std::string_view cookie(std::string_view key) const {
    return scan_kv(header("Cookie"), ';', key);
  }

  /**
   * Return the value of a field from an application/x-www-form-urlencoded body.
   * Returns empty when Content-Type does not match or the key is absent.
   */
  std::string_view form(std::string_view key) const {
    if (header("Content-Type").find("application/x-www-form-urlencoded")
        == std::string_view::npos)
      return {};
    return scan_kv(body_, '&', key);
  }

  // ── Setters used by the parser / router ────────────────────────────────────

  void set_method(Method m) noexcept { method_ = m; }

  /** Split the raw request-target at '?'; routing uses only path_. */
  void set_path(std::string_view raw) noexcept {
    size_t q = raw.find('?');
    if (q == std::string_view::npos) {
      path_         = raw;
      query_string_ = {};
    } else {
      path_         = raw.substr(0, q);
      query_string_ = raw.substr(q + 1);
    }
    query_cache_.reset(); // invalidate lazy cache on path change
  }

  void set_body(std::string_view b) noexcept { body_ = b; }

  /**
   * Store a header key/value pair. Both views MUST remain valid for the
   * lifetime of this Request (they reference the connection receive buffer).
   * Up to kMaxInlineHeaders entries are stored inline (zero allocation).
   */
  void add_header(std::string_view key, std::string_view value) noexcept {
    if (header_count_ < kMaxInlineHeaders) [[likely]]
      headers_flat_[header_count_++] = {key, value};
  }

  void set_param(std::string_view key, std::string_view value) noexcept {
    if (param_count_ < kMaxInlineParams) [[likely]]
      params_flat_[param_count_++] = {key, value};
  }

  /**
   * Store the remote (client) IP address as seen by the server.
   * Called by the server after accept(); NOT based on request headers.
   * Stored as an owned string because the socket address is ephemeral.
   */
  void set_remote_addr(std::string_view addr) {
    remote_addr_ = std::string(addr);
  }

private:
  /**
   * Shared key=value scanner used by query(), cookie(), and form().
   *
   * @param src   The string to scan (query string, Cookie header, or body).
   * @param sep   Pair separator ('&' for query/form, ';' for cookies).
   * @param key   The key to search for.
   */
  static std::string_view scan_kv(std::string_view src, char sep,
                                   std::string_view key) {
    while (!src.empty()) {
      // Skip leading whitespace (cookie values have "; " with a space)
      while (!src.empty() && src[0] == ' ')
        src.remove_prefix(1);

      size_t delim = src.find(sep);
      std::string_view pair = (delim == std::string_view::npos)
                                  ? src
                                  : src.substr(0, delim);

      size_t eq = pair.find('=');
      if (eq != std::string_view::npos && pair.substr(0, eq) == key)
        return pair.substr(eq + 1);

      if (delim == std::string_view::npos) break;
      src = src.substr(delim + 1);
    }
    return {};
  }

  // Lazy-populate query_cache_ from query_string_ on first query() call.
  void ensure_query_cache() const {
    if (query_cache_) return;
    query_cache_.emplace();

    // DoS guard: ignore query strings that are unreasonably large.
    // Legitimate query strings are rarely > 8 KiB; a 64 KiB limit stops
    // a single request from triggering a multi-megabyte parse.
    static constexpr size_t kMaxQueryStringBytes = 65536;
    if (query_string_.size() > kMaxQueryStringBytes) return;

    // DoS guard: limit the number of key-value pairs we will parse.
    static constexpr size_t kMaxQueryParams = 128;

    std::string_view src = query_string_;
    while (!src.empty()) {
      if (query_cache_->size() >= kMaxQueryParams) break;
      size_t delim = src.find('&');
      std::string_view pair = (delim == std::string_view::npos) ? src : src.substr(0, delim);
      size_t eq = pair.find('=');
      if (eq != std::string_view::npos) {
        (*query_cache_)[std::string(pair.substr(0, eq))] = std::string(pair.substr(eq + 1));
      }
      if (delim == std::string_view::npos) break;
      src = src.substr(delim + 1);
    }
  }

  // Maximum number of headers stored inline (zero allocation).
  // Typical HTTP requests have 5-15 headers; 32 covers all real-world cases.
  static constexpr uint8_t kMaxInlineHeaders = 32;
  // Maximum number of URL path parameters (:name segments).
  static constexpr uint8_t kMaxInlineParams  = 16;

  Method           method_       = Method::Unknown;
  // Zero-copy string_view fields — reference the connection receive buffer.
  // The buffer MUST outlive this Request object (standard HTTP server lifecycle).
  std::string_view path_;
  std::string_view query_string_;
  std::string_view body_;
  // Owned string — set by the server from socket data (ephemeral address struct).
  std::string      remote_addr_;

  // Inline flat-array header storage — zero heap allocation for all typical requests.
  // Key and value are string_views into the original receive buffer (zero-copy).
  // INVARIANT: the receive buffer MUST outlive this Request object.
  std::array<std::pair<std::string_view, std::string_view>, kMaxInlineHeaders>
      headers_flat_{};
  uint8_t header_count_ = 0;

  // Inline flat-array path-parameter storage.
  std::array<std::pair<std::string_view, std::string_view>, kMaxInlineParams>
      params_flat_{};
  uint8_t param_count_ = 0;

  mutable std::optional<StringMap> query_cache_; // lazy-parsed query string
};

} // namespace qbuem

/** @} */
