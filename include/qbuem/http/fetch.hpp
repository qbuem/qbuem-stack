#pragma once

/**
 * @file qbuem/http/fetch.hpp
 * @brief Monadic async HTTP client — curl-free, zero external dependencies.
 * @defgroup qbuem_http_fetch HTTP Fetch Client
 * @ingroup qbuem_http
 *
 * ## 설계 원칙
 * - **curl 불사용**: raw `TcpStream` + 직접 HTTP/1.1 직렬화/파싱
 * - **Monadic 체이닝**: `Result<FetchResponse>::map()` / `and_then()` 활용
 * - **Zero external dependencies**: qbuem 헤더만 사용
 * - **Coroutine-native**: `co_await fetch(...).send(st)` 패턴
 *
 * ## 사용 예시
 * @code
 * // 기본 GET 요청
 * auto resp = co_await fetch("http://httpbin.org/get").send(st);
 * if (!resp) co_return unexpected(resp.error());
 * std::string_view body = resp->body();
 *
 * // Monadic 체이닝
 * auto result = co_await fetch("http://api.example.com/users/1")
 *     .header("Accept", "application/json")
 *     .send(st);
 *
 * auto name = result
 *     .and_then([](const FetchResponse& r) -> Result<std::string> {
 *         if (!r.ok()) return unexpected(std::make_error_code(std::errc::protocol_error));
 *         return std::string(r.body());
 *     })
 *     .map([](const std::string& body) { return "got: " + body; })
 *     .value_or("error");
 *
 * // POST with body
 * auto post = co_await fetch("http://api.example.com/data")
 *     .method(Method::Post)
 *     .header("Content-Type", "application/json")
 *     .body(R"({"key":"value"})")
 *     .send(st);
 * @endcode
 *
 * ## 현재 제한사항
 * - HTTP/1.1 only (HTTP/2는 별도 grpc 채널 사용)
 * - HTTPS 미지원 (kTLS 통합은 별도 fetch_tls() 예정)
 * - Redirect 자동 미지원
 * - Chunked response body 지원 (Content-Length 또는 chunked encoding)
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/http/request.hpp>
#include <qbuem/net/socket_addr.hpp>
#include <qbuem/net/tcp_stream.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>

namespace qbuem {

// ─── ParsedUrl ────────────────────────────────────────────────────────────────

/**
 * @brief URL을 scheme / host / port / path+query로 분해한 구조체.
 *
 * 지원 형식:
 *   http://host/path?query
 *   http://host:port/path?query
 *
 * HTTPS는 현재 미지원 (향후 kTLS 통합 시 추가 예정).
 */
struct ParsedUrl {
  std::string scheme;  ///< "http" | "https"
  std::string host;    ///< 호스트명 또는 IP
  uint16_t    port;    ///< 포트 번호 (기본: http=80, https=443)
  std::string path;    ///< 경로 + 쿼리 ("/path?query" 형태, 없으면 "/")

  /**
   * @brief URL 문자열을 파싱하여 ParsedUrl을 반환합니다.
   *
   * @param url  파싱할 URL 문자열.
   * @returns    성공 시 ParsedUrl, 실패 시 errc::invalid_argument.
   */
  static Result<ParsedUrl> parse(std::string_view url) {
    ParsedUrl out;

    // scheme
    auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos)
      return unexpected(std::make_error_code(std::errc::invalid_argument));

    out.scheme = std::string(url.substr(0, scheme_end));
    url.remove_prefix(scheme_end + 3);

    // default port by scheme
    if (out.scheme == "http")       out.port = 80;
    else if (out.scheme == "https") out.port = 443;
    else return unexpected(std::make_error_code(std::errc::invalid_argument));

    // authority (host[:port]) vs path
    auto path_start = url.find('/');
    std::string_view authority = (path_start == std::string_view::npos)
                                     ? url
                                     : url.substr(0, path_start);

    if (path_start == std::string_view::npos) {
      out.path = "/";
    } else {
      out.path = std::string(url.substr(path_start));
    }

    // host:port split — handle IPv6 literal [::1]:port
    if (!authority.empty() && authority[0] == '[') {
      auto bracket = authority.find(']');
      if (bracket == std::string_view::npos)
        return unexpected(std::make_error_code(std::errc::invalid_argument));
      out.host = std::string(authority.substr(1, bracket - 1));
      authority.remove_prefix(bracket + 1);
      if (!authority.empty() && authority[0] == ':') {
        authority.remove_prefix(1);
        uint16_t p{};
        auto [ptr, ec] = std::from_chars(authority.data(),
                                         authority.data() + authority.size(), p);
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
        auto [ptr, ec] = std::from_chars(port_sv.data(),
                                          port_sv.data() + port_sv.size(), p);
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
 * @brief HTTP 클라이언트 응답 값 타입.
 *
 * `fetch().send()` 성공 시 반환됩니다.
 * Monadic 체이닝에서 `Result<FetchResponse>`의 내부 타입으로 사용됩니다.
 */
class FetchResponse {
public:
  FetchResponse() = default;

  /** @brief HTTP 상태 코드 (예: 200, 404, 500). */
  int status() const noexcept { return status_; }

  /** @brief 2xx 범위의 성공 응답인지 확인합니다. */
  bool ok() const noexcept { return status_ >= 200 && status_ < 300; }

  /** @brief 헤더 값을 반환합니다. 없으면 빈 string_view. */
  std::string_view header(std::string_view key) const {
    auto it = headers_.find(std::string(key));
    return (it != headers_.end()) ? std::string_view(it->second)
                                   : std::string_view{};
  }

  /** @brief 응답 바디 (raw bytes as string_view). */
  std::string_view body() const noexcept { return body_; }

  /** @brief 응답 바디 (소유권 이전). */
  std::string take_body() && { return std::move(body_); }

  // ── Setters (파서에서만 사용) ────────────────────────────────────────────
  void set_status(int s) noexcept { status_ = s; }
  void add_header(std::string_view k, std::string_view v) {
    // lowercase key for case-insensitive lookup
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

// ─── Internal: HTTP/1.1 serializer + response parser ─────────────────────────

namespace detail {

/**
 * @brief HTTP/1.1 요청 텍스트를 직렬화합니다.
 */
inline std::string serialize_request(Method method,
                                     const ParsedUrl &url,
                                     const StringMap &headers,
                                     std::string_view body) {
  std::string out;
  out.reserve(256 + body.size());

  // Request line
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

  // User headers
  for (auto &[k, v] : headers) {
    out += k; out += ": "; out += v; out += "\r\n";
  }

  // Content-Length if body present
  if (!body.empty()) {
    out += "Content-Length: ";
    out += std::to_string(body.size());
    out += "\r\n";
  }

  // Connection: close — simplest strategy (no keep-alive pool yet)
  out += "Connection: close\r\n";
  out += "\r\n";

  if (!body.empty()) out += body;
  return out;
}

/**
 * @brief 수신된 HTTP/1.1 응답 버퍼를 파싱하여 FetchResponse로 변환합니다.
 *
 * Content-Length 및 Transfer-Encoding: chunked 양쪽 처리.
 */
inline Result<FetchResponse> parse_response(std::string_view raw) {
  FetchResponse resp;

  // ── Status line ──────────────────────────────────────────────────────────
  auto crlf = raw.find("\r\n");
  if (crlf == std::string_view::npos)
    return unexpected(std::make_error_code(std::errc::protocol_error));

  std::string_view status_line = raw.substr(0, crlf);
  raw.remove_prefix(crlf + 2);

  // "HTTP/1.x NNN reason"
  if (status_line.size() < 12 ||
      (status_line.substr(0, 5) != "HTTP/"))
    return unexpected(std::make_error_code(std::errc::protocol_error));

  std::string_view code_sv = status_line.substr(9, 3); // "NNN"
  int code{};
  auto [ptr, ec] = std::from_chars(code_sv.data(), code_sv.data() + 3, code);
  if (ec != std::errc{})
    return unexpected(std::make_error_code(std::errc::protocol_error));
  resp.set_status(code);

  // ── Headers ──────────────────────────────────────────────────────────────
  while (true) {
    auto nl = raw.find("\r\n");
    if (nl == std::string_view::npos)
      return unexpected(std::make_error_code(std::errc::protocol_error));
    if (nl == 0) { // blank line = end of headers
      raw.remove_prefix(2);
      break;
    }
    std::string_view line = raw.substr(0, nl);
    raw.remove_prefix(nl + 2);

    auto colon = line.find(':');
    if (colon == std::string_view::npos) continue; // malformed header, skip

    std::string_view key = line.substr(0, colon);
    std::string_view val = line.substr(colon + 1);
    // trim leading whitespace from value
    while (!val.empty() && (val[0] == ' ' || val[0] == '\t'))
      val.remove_prefix(1);

    resp.add_header(key, val);
  }

  // ── Body ─────────────────────────────────────────────────────────────────
  std::string_view te = resp.header("transfer-encoding");
  if (te.find("chunked") != std::string_view::npos) {
    // Decode chunked body
    while (!raw.empty()) {
      auto nl = raw.find("\r\n");
      if (nl == std::string_view::npos) break;

      std::string_view size_line = raw.substr(0, nl);
      raw.remove_prefix(nl + 2);

      // Chunk size is hex; strip chunk extensions after ';'
      auto semi = size_line.find(';');
      if (semi != std::string_view::npos) size_line = size_line.substr(0, semi);

      size_t chunk_size = 0;
      for (char c : size_line) {
        chunk_size <<= 4;
        if (c >= '0' && c <= '9')      chunk_size |= (c - '0');
        else if (c >= 'a' && c <= 'f') chunk_size |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') chunk_size |= (c - 'A' + 10);
      }

      if (chunk_size == 0) break; // terminal chunk

      if (raw.size() < chunk_size + 2) break; // truncated
      resp.append_body(raw.substr(0, chunk_size));
      raw.remove_prefix(chunk_size + 2); // skip data + trailing CRLF
    }
  } else {
    // Content-Length based
    std::string_view cl_sv = resp.header("content-length");
    if (!cl_sv.empty()) {
      size_t cl = 0;
      std::from_chars(cl_sv.data(), cl_sv.data() + cl_sv.size(), cl);
      resp.set_body(std::string(raw.substr(0, cl)));
    } else {
      // No Content-Length and not chunked: read until connection close
      resp.set_body(std::string(raw));
    }
  }

  return resp;
}

/**
 * @brief TCP 스트림에서 HTTP 응답을 완전히 읽습니다.
 *
 * Content-Length / chunked / connection-close 세 가지 종료 방식 처리.
 */
inline Task<Result<std::string>> read_http_response(TcpStream &stream,
                                                     std::stop_token st) {
  static constexpr size_t kChunkSize = 4096;
  static constexpr size_t kMaxResponseSize = 8 * 1024 * 1024; // 8 MiB guard

  std::string buf;
  buf.reserve(kChunkSize);

  std::array<std::byte, kChunkSize> tmp{};

  size_t header_end = std::string::npos;
  size_t content_length = std::string::npos;
  bool   chunked = false;

  while (true) {
    if (st.stop_requested())
      co_return unexpected(std::make_error_code(std::errc::operation_canceled));

    auto n = co_await stream.read(tmp);
    if (!n) co_return unexpected(n.error());
    if (*n == 0) break; // EOF / connection closed by server

    buf.append(reinterpret_cast<const char *>(tmp.data()), *n);

    if (buf.size() > kMaxResponseSize)
      co_return unexpected(std::make_error_code(std::errc::message_size));

    // Parse header boundary on first discovery
    if (header_end == std::string::npos) {
      auto pos = buf.find("\r\n\r\n");
      if (pos != std::string::npos) {
        header_end = pos + 4;

        // Peek at headers to know when to stop reading
        std::string_view hdr_section(buf.data(), header_end);

        // Check Transfer-Encoding: chunked
        {
          std::string lower(hdr_section);
          std::transform(lower.begin(), lower.end(), lower.begin(),
                         [](unsigned char c) { return std::tolower(c); });
          if (lower.find("transfer-encoding: chunked") != std::string::npos)
            chunked = true;
        }

        if (!chunked) {
          // Try to find Content-Length
          auto cl_pos = hdr_section.find("Content-Length: ");
          if (cl_pos == std::string_view::npos)
            cl_pos = hdr_section.find("content-length: ");
          if (cl_pos != std::string_view::npos) {
            cl_pos += 16; // skip "Content-Length: "
            auto eol = hdr_section.find("\r\n", cl_pos);
            std::string_view cl_sv = hdr_section.substr(
                cl_pos,
                (eol == std::string_view::npos) ? std::string_view::npos
                                                 : eol - cl_pos);
            std::from_chars(cl_sv.data(), cl_sv.data() + cl_sv.size(),
                            content_length);
          }
        }
      }
    }

    // Early termination checks
    if (header_end != std::string::npos) {
      if (!chunked && content_length != std::string::npos) {
        // Check if we have enough body bytes
        if (buf.size() >= header_end + content_length)
          break;
      } else if (chunked) {
        // For chunked: check for terminal "0\r\n\r\n"
        if (buf.find("0\r\n\r\n") != std::string::npos)
          break;
      }
      // Otherwise (no Content-Length, not chunked): read until EOF
    }
  }

  co_return buf;
}

} // namespace detail

// ─── FetchRequest ─────────────────────────────────────────────────────────────

/**
 * @brief HTTP 클라이언트 요청 빌더.
 *
 * `fetch(url)` 팩토리로 생성하고 빌더 패턴으로 설정합니다.
 * 최종적으로 `co_await req.send(st)` 로 요청을 실행합니다.
 *
 * @code
 * auto resp = co_await fetch("http://httpbin.org/post")
 *     .method(Method::Post)
 *     .header("Content-Type", "application/json")
 *     .body(R"({"hello":"world"})")
 *     .send(st);
 * @endcode
 */
class FetchRequest {
public:
  explicit FetchRequest(std::string url) : url_(std::move(url)) {}

  /** @brief HTTP 메서드 설정 (기본: GET). */
  FetchRequest &method(Method m) { method_ = m; return *this; }

  /** @brief 헤더 추가. 여러 번 호출하여 누적 가능. */
  FetchRequest &header(std::string_view key, std::string_view value) {
    headers_[std::string(key)] = std::string(value);
    return *this;
  }

  /** @brief 요청 바디 설정. */
  FetchRequest &body(std::string_view b) { body_ = std::string(b); return *this; }

  /** @brief GET 메서드로 설정하는 단축 빌더. */
  FetchRequest &get()    { method_ = Method::Get;    return *this; }

  /** @brief POST 메서드로 설정하는 단축 빌더. */
  FetchRequest &post()   { method_ = Method::Post;   return *this; }

  /** @brief PUT 메서드로 설정하는 단축 빌더. */
  FetchRequest &put()    { method_ = Method::Put;    return *this; }

  /** @brief DELETE 메서드로 설정하는 단축 빌더. */
  FetchRequest &del()    { method_ = Method::Delete; return *this; }

  /** @brief PATCH 메서드로 설정하는 단축 빌더. */
  FetchRequest &patch()  { method_ = Method::Patch;  return *this; }

  /**
   * @brief 요청을 실행하고 응답을 코루틴으로 반환합니다.
   *
   * curl-free HTTP/1.1 클라이언트 구현:
   * 1. URL 파싱
   * 2. TcpStream::connect() (non-blocking)
   * 3. HTTP/1.1 요청 직렬화 → write()
   * 4. 응답 수신 (Content-Length / chunked 처리)
   * 5. 응답 파싱 → FetchResponse
   *
   * @param st  취소 토큰. 각 I/O 전 확인합니다.
   * @returns   성공 시 FetchResponse, 실패 시 error_code.
   */
  Task<Result<FetchResponse>> send(std::stop_token st = {}) {
    // 1. URL 파싱
    auto parsed = ParsedUrl::parse(url_);
    if (!parsed) co_return unexpected(parsed.error());

    if (parsed->scheme == "https")
      co_return unexpected(std::make_error_code(std::errc::protocol_not_supported));

    // 2. 주소 해석 + 연결
    auto addr_r = SocketAddr::from_ipv4(parsed->host.c_str(), parsed->port);
    if (!addr_r)
      co_return unexpected(addr_r.error());

    if (st.stop_requested())
      co_return unexpected(std::make_error_code(std::errc::operation_canceled));

    auto stream_r = co_await TcpStream::connect(*addr_r);
    if (!stream_r) co_return unexpected(stream_r.error());

    TcpStream stream = std::move(*stream_r);
    stream.set_nodelay(true);

    // 3. HTTP 요청 직렬화 + 전송
    std::string req_text = detail::serialize_request(method_, *parsed,
                                                      headers_, body_);

    std::string_view remaining(req_text);
    while (!remaining.empty()) {
      if (st.stop_requested())
        co_return unexpected(std::make_error_code(std::errc::operation_canceled));

      auto w = co_await stream.write(
          std::span<const std::byte>(
              reinterpret_cast<const std::byte *>(remaining.data()),
              remaining.size()));
      if (!w) co_return unexpected(w.error());
      remaining.remove_prefix(*w);
    }

    // 4. 응답 수신
    auto raw_r = co_await detail::read_http_response(stream, st);
    if (!raw_r) co_return unexpected(raw_r.error());

    // 5. 응답 파싱
    co_return detail::parse_response(*raw_r);
  }

private:
  std::string url_;
  Method      method_ = Method::Get;
  StringMap   headers_;
  std::string body_;
};

// ─── fetch() factory ─────────────────────────────────────────────────────────

/**
 * @brief HTTP fetch 요청을 생성하는 팩토리 함수.
 *
 * JavaScript의 `fetch()` API와 유사한 진입점입니다.
 *
 * @param url  요청할 URL 문자열 ("http://host/path" 형식).
 * @returns    체이닝 가능한 FetchRequest 빌더.
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
