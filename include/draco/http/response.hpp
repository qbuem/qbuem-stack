#pragma once

/**
 * @file draco/http/response.hpp
 * @brief HTTP 응답 빌더 — 상태 코드, 헤더, 바디 체인 API
 * @defgroup qbuem_http_response HTTP Response
 * @ingroup qbuem_http
 *
 * Response는 빌더 패턴. status(), header(), body() 메서드를 체이닝.
 * chunk()/end_chunks()로 chunked transfer, sendfile_path()로 zero-copy 파일 전송,
 * set_cookie()로 쿠키 설정.
 * @{
 */

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
   * @brief Append a chunk to a chunked-encoding response.
   *
   * Call chunk() one or more times instead of body() to stream a response
   * using HTTP/1.1 Transfer-Encoding: chunked.  The final response is
   * serialized with chunked framing; no Content-Length is emitted.
   *
   * Example:
   *   res.chunk("Hello, ").chunk("world!").end_chunks();
   */
  Response &chunk(std::string_view data);

  /**
   * @brief Finalize a chunked response (appends the terminal 0-length chunk).
   *
   * Must be called after all chunk() calls.  Sets Transfer-Encoding: chunked
   * and clears Content-Length from the response.
   */
  Response &end_chunks();

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

  /**
   * @brief Build only the HTTP response header (status line + headers).
   *
   * Used with writev() scatter-gather to send header + body in one syscall
   * without concatenating them into a single allocation.
   */
  std::string serialize_header() const;

  /**
   * @brief Opt-in zero-copy file serving via sendfile(2) on Linux.
   *
   * Instead of reading the file into body(), stores the filesystem path.
   * The send loop calls sendfile() after sending the HTTP header,
   * transferring file data directly from page-cache to socket — no user-space
   * copy and no heap allocation for the file body.
   *
   * Content-Length must be set before calling this if the file size is known.
   * On non-Linux platforms the caller is responsible for setting body() instead.
   *
   * @param path   Absolute or relative filesystem path to the file.
   * @param size   File size in bytes (used for Content-Length).
   */
  Response &sendfile_path(std::string_view path, size_t size);

  /** @brief True when send should use sendfile() rather than write(). */
  bool has_sendfile() const noexcept { return !sendfile_path_.empty(); }
  const std::string &get_sendfile_path() const noexcept { return sendfile_path_; }
  size_t sendfile_size() const noexcept { return sendfile_size_; }

  /** @brief True when response uses chunked transfer encoding. */
  bool is_chunked() const noexcept { return chunked_; }
  /** @brief Returns the pre-encoded chunked body (framing included). */
  std::string_view chunk_buf() const noexcept { return chunk_buf_; }

private:
  std::string_view status_to_string(int code) const;

  int         status_code_ = 200;
  std::unordered_map<std::string, std::string> headers_;
  std::vector<std::string> cookies_; // raw Set-Cookie values
  std::string body_;

  // Zero-copy sendfile path (empty = not set)
  std::string sendfile_path_;
  size_t      sendfile_size_ = 0;

  // Chunked transfer encoding accumulator
  bool        chunked_      = false;
  std::string chunk_buf_;   // encoded chunked body (framing included)
};

} // namespace draco

/** @} */
