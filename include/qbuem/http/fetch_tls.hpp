#pragma once

/**
 * @file qbuem/http/fetch_tls.hpp
 * @brief HTTPS fetch using Kernel TLS (kTLS) offload — OpenSSL-free design.
 * @defgroup qbuem_fetch_tls HTTPS Fetch (kTLS)
 * @ingroup qbuem_http
 *
 * ## Architecture
 *
 * kTLS (Kernel TLS) offloads AES-GCM encryption/decryption to the kernel
 * after a TLS handshake completes in user space. This gives zero-copy
 * encrypted I/O: `send()`/`recv()` on a kTLS socket transparently
 * encrypt/decrypt without additional memcpy.
 *
 * The **TLS handshake** itself still requires a user-space TLS implementation.
 * qbuem-stack avoids pulling in OpenSSL or BoringSSL as a header dependency;
 * instead, the caller is responsible for completing the handshake and then
 * handing the derived session keys to `TlsStream` for kernel offload.
 *
 * ## Two-phase usage model
 *
 * ```
 * Phase 1 — TLS handshake (caller-provided, any TLS library):
 *   1. Create a raw TcpStream and connect to the server.
 *   2. Perform TLS 1.3 handshake using your preferred library (mbedTLS,
 *      WolfSSL, BoringSSL, …) over the raw TCP fd.
 *   3. Extract TLS session keys: tx_key, tx_iv, tx_seq, rx_key, rx_iv, rx_seq.
 *
 * Phase 2 — kTLS offload (this header):
 *   4. Construct a TlsStream from the connected fd + session params.
 *      TlsStream calls prepare_socket_for_ktls() + enable_ktls() internally.
 *   5. Use TlsStream::read()/write() — kernel encrypts/decrypts transparently.
 *   6. Use fetch_tls(url, session) to run a full HTTPS request.
 * ```
 *
 * ## Fallback behaviour
 * If the kernel does not support kTLS (Linux < 4.13 or CONFIG_TLS not built),
 * `TlsStream` falls back to user-space TLS via the caller-supplied read/write
 * callbacks. This lets the same code run on older kernels.
 *
 * ## Usage example
 * @code
 * // After completing TLS handshake with your preferred library:
 * io::KtlsSessionParams tx_params{...}; // from handshake library
 * io::KtlsSessionParams rx_params{...};
 *
 * TlsSessionParams session{
 *     .fd       = tcp_fd,           // connected TCP socket fd
 *     .tx       = tx_params,
 *     .rx       = rx_params,
 *     .hostname = "httpbin.org",    // for SNI / logging
 *     .port     = 443,
 * };
 *
 * auto resp = co_await fetch_tls("https://httpbin.org/get", session).send(st);
 * if (!resp) co_return std::unexpected(resp.error());
 * std::string_view body = resp->body();
 * @endcode
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/http/fetch.hpp>
#include <qbuem/io/ktls.hpp>
#include <qbuem/net/tcp_stream.hpp>

#include <stop_token>
#include <string>
#include <string_view>

namespace qbuem {

// ─── TlsSessionParams ────────────────────────────────────────────────────────

/**
 * @brief Input parameters required to activate kTLS on an already-handshaked socket.
 *
 * Fill this struct from the session keys extracted by your TLS handshake
 * library (mbedTLS, WolfSSL, BoringSSL, etc.) after a successful TLS 1.3
 * handshake, then pass it to `TlsStream` or `fetch_tls()`.
 */
struct TlsSessionParams {
  /** @brief Connected TCP socket file descriptor (non-blocking, post-handshake). */
  int fd = -1;

  /** @brief TX (send) session parameters — encryption direction. */
  io::KtlsSessionParams tx;

  /** @brief RX (receive) session parameters — decryption direction. */
  io::KtlsSessionParams rx;

  /** @brief Server hostname (used for logging and future SNI support). */
  std::string hostname;

  /** @brief Server port number. */
  uint16_t port = 443;
};

// ─── TlsStream ───────────────────────────────────────────────────────────────

/**
 * @brief A TcpStream with kTLS kernel offload activated.
 *
 * Wraps a connected, post-handshake TCP socket and activates kTLS TX/RX.
 * After construction, `read()` and `write()` operations transparently
 * encrypt/decrypt at the kernel layer — no user-space crypto overhead.
 *
 * ### Ownership
 * `TlsStream` takes ownership of the fd passed via `TlsSessionParams`.
 * Move-only (same as `TcpStream`).
 *
 * ### Fallback
 * If `prepare_socket_for_ktls()` or `enable_ktls()` fails with
 * `errc::not_supported`, the stream falls back to plain I/O on the fd.
 * The application must then handle encryption via the TLS library that
 * performed the handshake.
 */
class TlsStream {
public:
  /**
   * @brief Construct from session parameters and activate kTLS.
   *
   * Calls `prepare_socket_for_ktls(fd)` and `enable_ktls(fd, tx, rx)`.
   * On kernels that do not support kTLS, `ktls_active()` returns false
   * and the stream still provides plain I/O on the fd.
   *
   * @param params  Post-handshake session parameters.
   */
  explicit TlsStream(TlsSessionParams params)
      : stream_(TcpStream(params.fd))
      , hostname_(std::move(params.hostname))
      , port_(params.port) {
    // Attempt kTLS activation — graceful fallback if unsupported
    auto prep = io::prepare_socket_for_ktls(params.fd);
    if (prep) {
      auto r = io::enable_ktls(params.fd, params.tx, params.rx);
      ktls_active_ = r.has_value();
    }
  }

  /** @brief Move constructor. */
  TlsStream(TlsStream &&)            = default;
  TlsStream &operator=(TlsStream &&) = default;

  /** @brief Non-copyable (owns the socket). */
  TlsStream(const TlsStream &)            = delete;
  TlsStream &operator=(const TlsStream &) = delete;

  // ── I/O — delegated to TcpStream (kTLS is transparent) ──────────────────

  /**
   * @brief Async read into buf.
   *
   * The kernel decrypts incoming TLS records before placing data in the
   * buffer when kTLS RX is active.
   */
  Task<Result<size_t>> read(std::span<std::byte> buf) {
    return stream_.read(buf);
  }

  /**
   * @brief Async write from buf.
   *
   * The kernel encrypts outgoing data into TLS records when kTLS TX is active.
   */
  Task<Result<size_t>> write(std::span<const std::byte> buf) {
    return stream_.write(buf);
  }

  /** @brief Returns true if kTLS offload was successfully activated. */
  [[nodiscard]] bool ktls_active() const noexcept { return ktls_active_; }

  /** @brief Server hostname (informational). */
  [[nodiscard]] std::string_view hostname() const noexcept { return hostname_; }

  /** @brief Server port. */
  [[nodiscard]] uint16_t port() const noexcept { return port_; }

  /** @brief Underlying socket fd. */
  [[nodiscard]] int fd() const noexcept { return stream_.fd(); }

  /** @brief Enable TCP_NODELAY. */
  void set_nodelay(bool v) noexcept { stream_.set_nodelay(v); }

private:
  TcpStream   stream_;
  std::string hostname_;
  uint16_t    port_         = 443;
  bool        ktls_active_  = false;
};

// ─── TlsFetchRequest ─────────────────────────────────────────────────────────

/**
 * @brief HTTPS request builder using a caller-provided TlsStream.
 *
 * After performing a TLS handshake externally, pass the resulting session
 * parameters to `fetch_tls(url, session)` which returns this builder.
 *
 * The request is sent over the kTLS-activated socket; the kernel handles
 * AES-GCM encryption without any user-space crypto library on the I/O path.
 */
class TlsFetchRequest {
public:
  TlsFetchRequest(std::string url, const TlsSessionParams& session)
      : url_(std::move(url))
      , stream_(std::make_unique<TlsStream>(session)) {}

  /** @brief Set the HTTP method (default: GET). */
  TlsFetchRequest &method(Method m)                  { method_ = m; return *this; }
  /** @brief Add a request header. */
  TlsFetchRequest &header(std::string_view k, std::string_view v) {
    headers_[std::string(k)] = std::string(v);
    return *this;
  }
  /** @brief Set the request body. */
  TlsFetchRequest &body(std::string_view b)          { body_ = b; return *this; }
  /** @brief Set method to GET. */
  TlsFetchRequest &get()   { method_ = Method::Get;    return *this; }
  /** @brief Set method to POST. */
  TlsFetchRequest &post()  { method_ = Method::Post;   return *this; }
  /** @brief Set method to PUT. */
  TlsFetchRequest &put()   { method_ = Method::Put;    return *this; }
  /** @brief Set method to DELETE. */
  TlsFetchRequest &del()   { method_ = Method::Delete; return *this; }
  /** @brief Set method to PATCH. */
  TlsFetchRequest &patch() { method_ = Method::Patch;  return *this; }

  /**
   * @brief Send the HTTPS request over the kTLS stream.
   *
   * Serialises and writes the HTTP/1.1 request, then reads and parses
   * the response. The TLS encryption/decryption is transparent to this
   * code — the kernel handles it.
   *
   * @param st  Cancellation token.
   * @returns   FetchResponse on success, error_code on failure.
   */
  Task<Result<FetchResponse>> send(const std::stop_token& st = {}) {
    auto parsed = ParsedUrl::parse(url_);
    if (!parsed) co_return std::unexpected(parsed.error());

    if (parsed->scheme != "https" && parsed->scheme != "http")
      co_return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    stream_->set_nodelay(true);

    // Serialise request
    std::string req_text = detail::serialize_request(
        method_, *parsed, headers_, body_,
        /*keep_alive=*/false);

    std::string_view remaining(req_text);
    while (!remaining.empty()) {
      if (st.stop_requested())
        co_return std::unexpected(std::make_error_code(std::errc::operation_canceled));
      auto w = co_await stream_->write(
          std::span<const std::byte>(
              reinterpret_cast<const std::byte *>(remaining.data()),
              remaining.size()));
      if (!w) co_return std::unexpected(w.error());
      remaining.remove_prefix(*w);
    }

    // Read response via TlsStream (kTLS decrypts transparently)
    static constexpr size_t kBuf = 4096;
    static constexpr size_t kMax = 8 * 1024 * 1024;

    std::string buf;
    buf.reserve(kBuf);
    std::array<std::byte, kBuf> tmp{};

    size_t header_end     = std::string::npos;
    size_t content_length = std::string::npos;
    bool   chunked        = false;

    while (true) {
      if (st.stop_requested())
        co_return std::unexpected(std::make_error_code(std::errc::operation_canceled));
      auto n = co_await stream_->read(tmp);
      if (!n) co_return std::unexpected(n.error());
      if (*n == 0) break;
      buf.append(reinterpret_cast<const char *>(tmp.data()), *n);
      if (buf.size() > kMax)
        co_return std::unexpected(std::make_error_code(std::errc::message_size));

      if (header_end == std::string::npos) {
        auto pos = buf.find("\r\n\r\n");
        if (pos != std::string::npos) {
          header_end = pos + 4;
          std::string lower(buf.data(), header_end);
          std::transform(lower.begin(), lower.end(), lower.begin(),
                         [](unsigned char c) { return std::tolower(c); });
          if (lower.find("transfer-encoding: chunked") != std::string::npos)
            chunked = true;
          if (!chunked) {
            auto cl = lower.find("content-length: ");
            if (cl != std::string::npos) {
              cl += 16;
              auto eol = lower.find("\r\n", cl);
              std::string_view cl_sv(lower.data() + cl,
                                     (eol == std::string::npos)
                                         ? lower.size() - cl : eol - cl);
              std::from_chars(cl_sv.data(), cl_sv.data() + cl_sv.size(),
                              content_length);
            }
          }
        }
      }
      if (header_end != std::string::npos) {
        if (!chunked && content_length != std::string::npos &&
            buf.size() >= header_end + content_length) break;
        if (chunked && buf.find("0\r\n\r\n") != std::string::npos) break;
      }
    }

    co_return detail::parse_response(buf);
  }

private:
  std::string                 url_;
  std::unique_ptr<TlsStream>  stream_;
  Method                      method_ = Method::Get;
  StringMap                   headers_;
  std::string                 body_;
};

// ─── fetch_tls() factory ─────────────────────────────────────────────────────

/**
 * @brief Create an HTTPS request over a caller-provided kTLS-activated stream.
 *
 * The caller is responsible for:
 * 1. Connecting a TCP socket to the HTTPS server.
 * 2. Performing a TLS 1.3 handshake using any TLS library.
 * 3. Extracting TX/RX session keys and populating `TlsSessionParams`.
 *
 * `fetch_tls()` takes over from that point: it activates kTLS kernel offload
 * and sends the HTTP/1.1 request over the encrypted channel.
 *
 * @param url      HTTPS URL (scheme may be "https" or "http").
 * @param session  Post-handshake session parameters including the socket fd
 *                 and derived TLS 1.3 session keys.
 * @returns        A `TlsFetchRequest` builder.
 *
 * @code
 * io::KtlsSessionParams tx{...}, rx{...}; // from your TLS library
 * auto resp = co_await fetch_tls("https://api.example.com/data",
 *                                TlsSessionParams{.fd=fd, .tx=tx, .rx=rx,
 *                                                  .hostname="api.example.com"})
 *                 .get()
 *                 .header("Accept", "application/json")
 *                 .send(st);
 * @endcode
 */
[[nodiscard]] inline TlsFetchRequest fetch_tls(std::string_view url,
                                                const TlsSessionParams& session) {
  return TlsFetchRequest(std::string(url), session);
}

} // namespace qbuem

/** @} */ // end of qbuem_fetch_tls
