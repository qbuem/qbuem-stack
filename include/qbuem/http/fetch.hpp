#pragma once

/**
 * @file qbuem/http/fetch.hpp
 * @brief Monadic async HTTP client — curl-free, zero external dependencies.
 * @defgroup qbuem_http_fetch HTTP Fetch Client
 * @ingroup qbuem_http
 *
 * ## Design principles
 * - **curl-free**: raw `TcpStream` + hand-written HTTP/1.1 serialisation/parsing.
 * - **Monadic chaining**: leverage `Result<FetchResponse>::map()` / `and_then()`.
 * - **Zero external dependencies**: only qbuem headers are required.
 * - **Coroutine-native**: `co_await fetch(...).send(st)` pattern.
 * - **Async DNS**: hostname resolution via `DnsResolver` (non-blocking).
 * - **Timeout**: reactor-integrated timer; combined with caller's stop_token.
 * - **Redirect**: automatic 3xx following up to a configurable limit.
 *
 * ## Usage
 * @code
 * // Basic GET
 * auto resp = co_await fetch("http://httpbin.org/get").send(st);
 * if (!resp) co_return unexpected(resp.error());
 *
 * // Monadic chaining
 * auto result = co_await fetch("http://api.example.com/users/1")
 *     .header("Accept", "application/json")
 *     .timeout(std::chrono::seconds{5})
 *     .send(st);
 *
 * auto name = result
 *     .and_then([](const FetchResponse& r) -> Result<std::string> {
 *         if (!r.ok()) return unexpected(std::make_error_code(std::errc::protocol_error));
 *         return std::string(r.body());
 *     })
 *     .map([](const std::string& body) { return "got: " + body; })
 *     .value_or("(error)");
 *
 * // POST with body
 * auto post = co_await fetch("http://api.example.com/data")
 *     .method(Method::Post)
 *     .header("Content-Type", "application/json")
 *     .body(R"({"key":"value"})")
 *     .max_redirects(5)
 *     .send(st);
 * @endcode
 *
 * ## Current limitations
 * - HTTP/1.1 only (HTTP/2 is handled by the separate gRPC channel).
 * - HTTPS requires `fetch_tls()` from `<qbuem/http/fetch_tls.hpp>`.
 * - Chunked *request* bodies are not yet supported.
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/http/request.hpp>
#include <qbuem/net/dns.hpp>
#include <qbuem/net/socket_addr.hpp>
#include <qbuem/net/tcp_stream.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace qbuem {

// ─── ParsedUrl ────────────────────────────────────────────────────────────────

/**
 * @brief URL decomposed into scheme / host / port / path+query.
 *
 * Supported formats:
 *   http://host/path?query
 *   http://host:port/path?query
 *   http://[::1]:port/path   (IPv6 literal)
 *   https://host/path        (HTTPS — handled by fetch_tls())
 */
struct ParsedUrl {
  std::string scheme;  ///< "http" | "https"
  std::string host;    ///< Hostname or IP address string
  uint16_t    port;    ///< Port number (default: http=80, https=443)
  std::string path;    ///< Path + query ("/path?query", "/" if absent)

  /**
   * @brief Parse a URL string into a `ParsedUrl`.
   *
   * @param url  URL string to parse.
   * @returns    ParsedUrl on success, errc::invalid_argument on failure.
   */
  static Result<ParsedUrl> parse(std::string_view url) {
    ParsedUrl out;

    // Scheme
    auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos)
      return unexpected(std::make_error_code(std::errc::invalid_argument));

    out.scheme = std::string(url.substr(0, scheme_end));
    url.remove_prefix(scheme_end + 3);

    if (out.scheme == "http")       out.port = 80;
    else if (out.scheme == "https") out.port = 443;
    else return unexpected(std::make_error_code(std::errc::invalid_argument));

    // Authority vs path
    auto path_start = url.find('/');
    std::string_view authority = (path_start == std::string_view::npos)
                                     ? url
                                     : url.substr(0, path_start);
    out.path = (path_start == std::string_view::npos)
                   ? std::string("/")
                   : std::string(url.substr(path_start));

    // Host[:port] — handle IPv6 literals [::1]:port
    if (!authority.empty() && authority[0] == '[') {
      auto bracket = authority.find(']');
      if (bracket == std::string_view::npos)
        return unexpected(std::make_error_code(std::errc::invalid_argument));
      out.host = std::string(authority.substr(1, bracket - 1));
      authority.remove_prefix(bracket + 1);
      if (!authority.empty() && authority[0] == ':') {
        authority.remove_prefix(1);
        uint16_t p{};
        auto [ptr, ec] = std::from_chars(
            authority.data(), authority.data() + authority.size(), p);
        if (ec != std::errc{})
          return unexpected(std::make_error_code(std::errc::invalid_argument));
        out.port = p;
      }
    } else {
      auto colon = authority.find(':');
      if (colon == std::string_view::npos) {
        out.host = std::string(authority);
      } else {
        out.host = std::string(authority.substr(0, colon));
        std::string_view port_sv = authority.substr(colon + 1);
        uint16_t p{};
        auto [ptr, ec] = std::from_chars(
            port_sv.data(), port_sv.data() + port_sv.size(), p);
        if (ec != std::errc{})
          return unexpected(std::make_error_code(std::errc::invalid_argument));
        out.port = p;
      }
    }

    if (out.host.empty())
      return unexpected(std::make_error_code(std::errc::invalid_argument));

    return out;
  }
};

// ─── FetchResponse ────────────────────────────────────────────────────────────

/**
 * @brief HTTP client response value type.
 *
 * Returned inside `Result<FetchResponse>` by `FetchRequest::send()`.
 * Supports monadic chaining via the inherited `Result<T>` operations.
 */
class FetchResponse {
public:
  FetchResponse() = default;

  /** @brief HTTP status code (e.g. 200, 404, 500). */
  int status() const noexcept { return status_; }

  /** @brief Returns true for 2xx success responses. */
  bool ok() const noexcept { return status_ >= 200 && status_ < 300; }

  /**
   * @brief Return a response header value (case-insensitive key lookup).
   * @returns Header value, or empty string_view if the header is absent.
   */
  std::string_view header(std::string_view key) const {
    // Keys are stored pre-lowercased by add_header(); just lowercase
    // the (typically short) lookup key without a heap allocation.
    char  buf[64];
    const bool fits = key.size() < sizeof(buf);
    if (fits) {
      for (size_t i = 0; i < key.size(); ++i)
        buf[i] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(key[i])));
      std::string_view lower_key(buf, key.size());
      auto it = headers_.find(std::string(lower_key));
      return (it != headers_.end()) ? std::string_view(it->second)
                                     : std::string_view{};
    }
    std::string lower(key);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    auto it = headers_.find(lower);
    return (it != headers_.end()) ? std::string_view(it->second)
                                   : std::string_view{};
  }

  /** @brief Response body as a string_view (raw bytes). */
  std::string_view body() const noexcept { return body_; }

  /** @brief Move the response body out (avoids a copy). */
  std::string take_body() && { return std::move(body_); }

  // ── Setters used by the parser ───────────────────────────────────────────
  void set_status(int s) noexcept { status_ = s; }
  void add_header(std::string_view k, std::string_view v) {
    std::string key(k);
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    headers_[std::move(key)] = std::string(v);
  }
  void set_body(std::string b) { body_ = std::move(b); }
  void append_body(std::string_view chunk) { body_ += chunk; }

private:
  int status_ = 0;
  std::unordered_map<std::string, std::string> headers_;
  std::string body_;
};

// ─── Internal helpers ─────────────────────────────────────────────────────────

namespace detail {

/** @brief Serialise an outbound HTTP/1.1 request to a string. */
inline std::string serialize_request(Method method,
                                     const ParsedUrl &url,
                                     const StringMap &headers,
                                     std::string_view body,
                                     bool keep_alive) {
  std::string out;
  out.reserve(256 + body.size());

  switch (method) {
  case Method::Get:     out += "GET ";     break;
  case Method::Post:    out += "POST ";    break;
  case Method::Put:     out += "PUT ";     break;
  case Method::Delete:  out += "DELETE ";  break;
  case Method::Patch:   out += "PATCH ";   break;
  case Method::Options: out += "OPTIONS "; break;
  case Method::Head:    out += "HEAD ";    break;
  default:              out += "GET ";     break;
  }

  out += url.path.empty() ? "/" : url.path;
  out += " HTTP/1.1\r\n";

  // Host header (mandatory in HTTP/1.1)
  out += "Host: ";
  out += url.host;
  if ((url.scheme == "http"  && url.port != 80) ||
      (url.scheme == "https" && url.port != 443)) {
    out += ':';
    out += std::to_string(url.port);
  }
  out += "\r\n";

  // User-supplied headers
  for (auto &[k, v] : headers) {
    out += k; out += ": "; out += v; out += "\r\n";
  }

  // Content-Length when a body is present
  if (!body.empty()) {
    out += "Content-Length: ";
    out += std::to_string(body.size());
    out += "\r\n";
  }

  out += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
  out += "\r\n";
  if (!body.empty()) out += body;
  return out;
}

/**
 * @brief ASCII case-insensitive substring search (header section only).
 *
 * Avoids the full-buffer tolower() copy used previously.  Only searches
 * within [data, data + len) so we don't scan past the header boundary.
 *
 * @param haystack  Pointer to the raw header bytes.
 * @param hlen      Length of the header region to search.
 * @param needle    Lower-case search string.
 * @returns         Pointer to the first match, or nullptr if not found.
 */
inline const char *icase_find(const char *haystack, size_t hlen,
                               std::string_view needle) noexcept {
  if (needle.empty() || needle.size() > hlen) return nullptr;
  const size_t stop = hlen - needle.size();
  for (size_t i = 0; i <= stop; ++i) {
    bool match = true;
    for (size_t j = 0; j < needle.size(); ++j) {
      if (std::tolower(static_cast<unsigned char>(haystack[i + j]))
          != static_cast<unsigned char>(needle[j])) {
        match = false;
        break;
      }
    }
    if (match) return haystack + i;
  }
  return nullptr;
}

/** @brief Parse a raw HTTP/1.1 response buffer into a FetchResponse. */
inline Result<FetchResponse> parse_response(std::string_view raw) {
  FetchResponse resp;

  // Status line
  auto crlf = raw.find("\r\n");
  if (crlf == std::string_view::npos)
    return unexpected(std::make_error_code(std::errc::protocol_error));

  std::string_view status_line = raw.substr(0, crlf);
  raw.remove_prefix(crlf + 2);

  if (status_line.size() < 12 || status_line.substr(0, 5) != "HTTP/")
    return unexpected(std::make_error_code(std::errc::protocol_error));

  int code{};
  auto [ptr, ec] = std::from_chars(
      status_line.data() + 9, status_line.data() + 12, code);
  if (ec != std::errc{})
    return unexpected(std::make_error_code(std::errc::protocol_error));
  resp.set_status(code);

  // Headers
  while (true) {
    auto nl = raw.find("\r\n");
    if (nl == std::string_view::npos)
      return unexpected(std::make_error_code(std::errc::protocol_error));
    if (nl == 0) { raw.remove_prefix(2); break; } // blank line = end of headers

    std::string_view line = raw.substr(0, nl);
    raw.remove_prefix(nl + 2);

    auto colon = line.find(':');
    if (colon == std::string_view::npos) continue;

    std::string_view key = line.substr(0, colon);
    std::string_view val = line.substr(colon + 1);
    while (!val.empty() && (val[0] == ' ' || val[0] == '\t'))
      val.remove_prefix(1);
    resp.add_header(key, val);
  }

  // Body: chunked or Content-Length or read-until-close
  std::string_view te = resp.header("transfer-encoding");
  if (te.find("chunked") != std::string_view::npos) {
    while (!raw.empty()) {
      auto nl = raw.find("\r\n");
      if (nl == std::string_view::npos) break;
      std::string_view size_line = raw.substr(0, nl);
      raw.remove_prefix(nl + 2);
      auto semi = size_line.find(';');
      if (semi != std::string_view::npos) size_line = size_line.substr(0, semi);

      size_t chunk_size = 0;
      for (char c : size_line) {
        chunk_size <<= 4;
        if      (c >= '0' && c <= '9') chunk_size |= (c - '0');
        else if (c >= 'a' && c <= 'f') chunk_size |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') chunk_size |= (c - 'A' + 10);
      }
      if (chunk_size == 0) break;
      if (raw.size() < chunk_size + 2) break;
      resp.append_body(raw.substr(0, chunk_size));
      raw.remove_prefix(chunk_size + 2);
    }
  } else {
    std::string_view cl_sv = resp.header("content-length");
    if (!cl_sv.empty()) {
      size_t cl = 0;
      std::from_chars(cl_sv.data(), cl_sv.data() + cl_sv.size(), cl);
      resp.set_body(std::string(raw.substr(0, cl)));
    } else {
      resp.set_body(std::string(raw));
    }
  }

  return resp;
}

/**
 * @brief Read a complete HTTP/1.1 response from a TcpStream.
 *
 * Handles Content-Length, Transfer-Encoding: chunked, and connection-close
 * termination. Enforces an 8 MiB guard to prevent unbounded allocation.
 */
inline Task<Result<std::string>> read_http_response(TcpStream &stream,
                                                     std::stop_token st) {
  static constexpr size_t kChunkSize       = 4096;
  static constexpr size_t kMaxResponseSize = 8 * 1024 * 1024; // 8 MiB

  std::string buf;
  buf.reserve(kChunkSize);

  // Allocated once outside the loop — avoids stack re-init per iteration.
  std::array<std::byte, kChunkSize> tmp;

  size_t header_end      = std::string::npos;
  size_t content_length  = std::string::npos;
  bool   chunked         = false;

  while (true) {
    if (st.stop_requested())
      co_return unexpected(std::make_error_code(std::errc::operation_canceled));

    auto n = co_await stream.read(tmp);
    if (!n) co_return unexpected(n.error());
    if (*n == 0) break; // EOF / server closed connection

    buf.append(reinterpret_cast<const char *>(tmp.data()), *n);

    if (buf.size() > kMaxResponseSize)
      co_return unexpected(std::make_error_code(std::errc::message_size));

    // Parse header boundary on first discovery.
    // Use icase_find() to avoid creating a full lowercase copy of the
    // header section on every iteration.
    if (header_end == std::string::npos) {
      auto pos = buf.find("\r\n\r\n");
      if (pos != std::string::npos) {
        header_end = pos + 4;

        if (icase_find(buf.data(), header_end, "transfer-encoding: chunked"))
          chunked = true;

        if (!chunked) {
          const char *cl_ptr =
              icase_find(buf.data(), header_end, "content-length: ");
          if (cl_ptr) {
            cl_ptr += 16; // skip "content-length: "
            const char *eol =
                static_cast<const char *>(
                    ::memchr(cl_ptr, '\r', buf.data() + header_end - cl_ptr));
            size_t cl_len = eol ? static_cast<size_t>(eol - cl_ptr)
                                : static_cast<size_t>(buf.data() + header_end - cl_ptr);
            std::from_chars(cl_ptr, cl_ptr + cl_len, content_length);
            // Pre-allocate based on Content-Length to avoid incremental growth.
            if (content_length != std::string::npos &&
                content_length < kMaxResponseSize) {
              buf.reserve(header_end + content_length);
            }
          }
        }
      }
    }

    // Early exit when we have enough data.
    if (header_end != std::string::npos) {
      if (!chunked && content_length != std::string::npos) {
        if (buf.size() >= header_end + content_length) break;
      } else if (chunked) {
        if (buf.find("0\r\n\r\n") != std::string::npos) break;
      }
    }
  }

  co_return buf;
}

/**
 * @brief Helper: combine two stop_tokens into one.
 *
 * The combined source fires when either input fires.
 * Ownership of the callbacks must survive until after the combined
 * token is no longer needed.
 */
struct CombinedStop {
  std::stop_source source;
  std::stop_callback<std::function<void()>> cb_a;
  std::stop_callback<std::function<void()>> cb_b;

  CombinedStop(std::stop_token a, std::stop_token b)
      : cb_a(a, [this] { source.request_stop(); })
      , cb_b(b, [this] { source.request_stop(); }) {}

  std::stop_token token() { return source.get_token(); }
};

} // namespace detail

// ─── FetchRequest ─────────────────────────────────────────────────────────────

/**
 * @brief HTTP client request builder.
 *
 * Created by `fetch(url)`. Configure the request with builder methods then
 * execute it with `co_await req.send(st)`.
 *
 * @code
 * auto resp = co_await fetch("http://httpbin.org/post")
 *     .method(Method::Post)
 *     .header("Content-Type", "application/json")
 *     .body(R"({"hello":"world"})")
 *     .timeout(std::chrono::seconds{10})
 *     .max_redirects(5)
 *     .send(st);
 * @endcode
 */
class FetchRequest {
public:
  explicit FetchRequest(std::string url) : url_(std::move(url)) {}

  /** @brief Set the HTTP method (default: GET). */
  FetchRequest &method(Method m)      { method_ = m; return *this; }

  /** @brief Add a request header. Can be called multiple times. */
  FetchRequest &header(std::string_view key, std::string_view value) {
    headers_[std::string(key)] = std::string(value);
    return *this;
  }

  /** @brief Set the request body. */
  FetchRequest &body(std::string_view b) { body_ = b; return *this; }

  /** @brief Set method to GET (convenience shorthand). */
  FetchRequest &get()    { method_ = Method::Get;    return *this; }
  /** @brief Set method to POST (convenience shorthand). */
  FetchRequest &post()   { method_ = Method::Post;   return *this; }
  /** @brief Set method to PUT (convenience shorthand). */
  FetchRequest &put()    { method_ = Method::Put;    return *this; }
  /** @brief Set method to DELETE (convenience shorthand). */
  FetchRequest &del()    { method_ = Method::Delete; return *this; }
  /** @brief Set method to PATCH (convenience shorthand). */
  FetchRequest &patch()  { method_ = Method::Patch;  return *this; }

  /**
   * @brief Set a request timeout.
   *
   * If the request (including DNS + connect + all I/O) does not complete within
   * this duration, `send()` returns `errc::timed_out` (or `operation_canceled`
   * if the timeout fires via the reactor timer).
   *
   * A zero or negative duration disables the timeout (default behaviour).
   *
   * @param d  Duration until timeout.
   */
  FetchRequest &timeout(std::chrono::milliseconds d) {
    timeout_ms_ = d.count();
    return *this;
  }

  /** @brief Overload accepting any duration type. */
  template <typename Rep, typename Period>
  FetchRequest &timeout(std::chrono::duration<Rep, Period> d) {
    timeout_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
    return *this;
  }

  /**
   * @brief Set the maximum number of 3xx redirects to follow (default: 0 — no redirect).
   * @param n  Maximum redirect hops; 0 means do not follow redirects.
   */
  FetchRequest &max_redirects(int n) { max_redirects_ = n; return *this; }

  /**
   * @brief Execute the HTTP request and return the response.
   *
   * Algorithm:
   *  1. Parse URL.
   *  2. Async DNS resolution (`DnsResolver::resolve`).
   *  3. Non-blocking TCP connect (`TcpStream::connect`).
   *  4. Serialise HTTP/1.1 request and write to socket.
   *  5. Read full response (Content-Length / chunked / connection-close).
   *  6. Parse response into `FetchResponse`.
   *  7. If 3xx and redirects remain, repeat from step 1 with the Location header.
   *
   * If a timeout was set, a reactor timer fires and requests cancellation via
   * a combined stop_token. The caller's stop_token is also respected.
   *
   * @param st  Cancellation token. Checked before every I/O operation.
   * @returns   FetchResponse on success, or an error_code on failure.
   */
  Task<Result<FetchResponse>> send(std::stop_token st = {}) {
    // ── Build combined stop_token (user + optional timeout timer) ───────
    std::stop_source  timeout_ss;
    int               timer_id = -1;
    Reactor*          reactor  = Reactor::current();

    if (timeout_ms_ > 0 && reactor) {
      auto r = reactor->register_timer(
          static_cast<int>(timeout_ms_),
          [&timeout_ss](int) { timeout_ss.request_stop(); });
      if (r) timer_id = *r;
    }

    detail::CombinedStop combined{st, timeout_ss.get_token()};
    auto effective_st = combined.token();

    // ── Redirect loop ────────────────────────────────────────────────────
    std::string current_url = url_;
    int         redirects   = 0;

    while (true) {
      auto result = co_await send_one(current_url, effective_st);

      if (!result) {
        // Cancel timer before returning
        if (timer_id >= 0 && reactor) reactor->unregister_timer(timer_id);
        co_return result;
      }

      int status = result->status();
      bool is_redirect = (status == 301 || status == 302 ||
                          status == 303 || status == 307 || status == 308);

      if (is_redirect && redirects < max_redirects_) {
        std::string_view location = result->header("location");
        if (!location.empty()) {
          current_url = std::string(location);
          ++redirects;
          // 303 See Other: subsequent request is always GET
          if (status == 303) method_ = Method::Get;
          continue;
        }
      }

      // Done — cancel timer and return
      if (timer_id >= 0 && reactor) reactor->unregister_timer(timer_id);
      co_return result;
    }
  }

private:
  /**
   * @brief Perform a single HTTP/1.1 request (no redirect logic).
   *
   * @param url  Fully qualified URL to request.
   * @param st   Effective stop_token (combined user + timeout).
   */
  Task<Result<FetchResponse>> send_one(const std::string &url,
                                       std::stop_token st) {
    // Parse URL
    auto parsed = ParsedUrl::parse(url);
    if (!parsed) co_return unexpected(parsed.error());

    if (parsed->scheme == "https")
      co_return unexpected(
          std::make_error_code(std::errc::protocol_not_supported));

    if (st.stop_requested())
      co_return unexpected(std::make_error_code(std::errc::operation_canceled));

    // Async DNS resolution
    auto addr_r = co_await DnsResolver::resolve(parsed->host, parsed->port);
    if (!addr_r) co_return unexpected(addr_r.error());

    if (st.stop_requested())
      co_return unexpected(std::make_error_code(std::errc::operation_canceled));

    // TCP connect
    auto stream_r = co_await TcpStream::connect(*addr_r);
    if (!stream_r) co_return unexpected(stream_r.error());

    TcpStream stream = std::move(*stream_r);
    stream.set_nodelay(true);

    // Serialise and write request
    std::string req_text = detail::serialize_request(
        method_, *parsed, headers_, body_,
        /*keep_alive=*/false);

    std::string_view remaining(req_text);
    while (!remaining.empty()) {
      if (st.stop_requested())
        co_return unexpected(
            std::make_error_code(std::errc::operation_canceled));
      auto w = co_await stream.write(
          std::span<const std::byte>(
              reinterpret_cast<const std::byte *>(remaining.data()),
              remaining.size()));
      if (!w) co_return unexpected(w.error());
      remaining.remove_prefix(*w);
    }

    // Read response
    auto raw_r = co_await detail::read_http_response(stream, st);
    if (!raw_r) co_return unexpected(raw_r.error());

    co_return detail::parse_response(*raw_r);
  }

  std::string url_;
  Method      method_        = Method::Get;
  StringMap   headers_;
  std::string body_;
  long long   timeout_ms_    = 0;   // 0 = no timeout
  int         max_redirects_ = 0;   // 0 = do not follow redirects
};

// ─── fetch() factory ─────────────────────────────────────────────────────────

/**
 * @brief Create an HTTP fetch request — the main entry point.
 *
 * Mirrors the browser `fetch()` API. Returns a chainable `FetchRequest`
 * builder; call `co_await .send(st)` to execute.
 *
 * @param url  Target URL ("http://host/path" format).
 * @returns    A `FetchRequest` builder.
 *
 * @code
 * // GET
 * auto r = co_await fetch("http://httpbin.org/get").send(st);
 *
 * // POST + monadic chaining
 * auto status = co_await fetch("http://api.example.com/items")
 *     .post()
 *     .header("Content-Type", "application/json")
 *     .body(payload)
 *     .timeout(std::chrono::seconds{5})
 *     .send(st);
 *
 * int code = status.map([](const FetchResponse& r){ return r.status(); })
 *                  .value_or(-1);
 * @endcode
 */
[[nodiscard]] inline FetchRequest fetch(std::string_view url) {
  return FetchRequest(std::string(url));
}

} // namespace qbuem

/** @} */ // end of qbuem_http_fetch
