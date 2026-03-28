#pragma once

/**
 * @file qbuem/server/http1_handler.hpp
 * @brief HTTP/1.1 connection handler — IConnectionHandler<http::Request> implementation
 * @defgroup qbuem_http1_handler HTTP/1.1 Handler
 * @ingroup qbuem_server
 *
 * This header provides a handler that manages the connection lifecycle for the HTTP/1.1 protocol.
 *
 * ### Key features
 * - **keep-alive**: Checks the `Connection` header to reuse connections.
 * - **100-continue**: Sends `100 Continue` before receiving the body when `Expect: 100-continue` is detected.
 * - **Router injection**: Accepts an `http::Router` in the constructor to route requests.
 * - **WebSocket upgrade detection**: Returns a special result when the `Upgrade: websocket` header is received.
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/http/request.hpp>
#include <qbuem/http/response.hpp>
#include <qbuem/http/router.hpp>
#include <qbuem/io/scattered_span.hpp>
#include <qbuem/net/socket_addr.hpp>
#include <qbuem/server/connection_handler.hpp>

#include <algorithm>
#include <cerrno>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/uio.h>
#include <unistd.h>

namespace qbuem {

// ─── UpgradeRequest ──────────────────────────────────────────────────────────

/**
 * @brief Struct holding WebSocket upgrade request information.
 *
 * Context passed to the upper layer when `Http1Handler::on_frame()`
 * detects an `Upgrade: websocket` header.
 */
struct UpgradeRequest {
  /** @brief Original HTTP request that initiated the upgrade. */
  http::Request original_request;
  /** @brief Connected socket file descriptor. */
  int fd{-1};
};

// ─── Http1Handler ────────────────────────────────────────────────────────────

/**
 * @brief HTTP/1.1 protocol connection handler.
 *
 * Implements the `IConnectionHandler<http::Request>` interface and handles
 * HTTP/1.1 keep-alive, 100-continue, and WebSocket upgrades.
 *
 * ### Connection lifecycle
 * 1. `on_connect()` — Stores the socket fd and remote address.
 * 2. `on_frame()` — Dispatches each parsed request to the Router.
 *    - Sets `keep_alive_` to false if `Connection: close` is present.
 *    - Sends `100 Continue` before receiving the body if `Expect: 100-continue` is present.
 *    - Invokes the upgrade callback if `Upgrade: websocket` is present.
 * 3. `on_disconnect()` — Cleans up resources.
 *
 * ### Usage example
 * @code
 * auto router = std::make_shared<http::Router>();
 * router->add_route(Method::Get, "/hello",
 *     [](const Request& req, Response& res) {
 *         res.status(200).body("Hello, World!");
 *     });
 *
 * auto factory = [router]{ return std::make_unique<Http1Handler>(router); };
 * AcceptLoop<http::Request, decltype(factory)> loop({
 *     .addr    = *SocketAddr::from_ipv4("0.0.0.0", 8080),
 *     .factory = factory,
 * });
 * co_await loop.run();
 * @endcode
 */
class Http1Handler : public IConnectionHandler<http::Request> {
public:
  /**
   * @brief Callback type for handling WebSocket upgrade requests.
   *
   * This callback is invoked when an `Upgrade: websocket` header is received.
   * In the callback, take ownership of the connection and hand it off to a WebSocketHandler.
   */
  using UpgradeCallback = std::function<Task<void>(UpgradeRequest)>;

  /**
   * @brief Constructs an Http1Handler.
   *
   * @param router            Router instance used to route requests.
   * @param upgrade_callback  Callback invoked on WebSocket upgrade request.
   *                          If nullptr, the upgrade is rejected and `400 Upgrade Required` is returned.
   */
  explicit Http1Handler(std::shared_ptr<http::Router> router,
                        UpgradeCallback upgrade_callback = nullptr)
      : router_(std::move(router))
      , upgrade_callback_(std::move(upgrade_callback)) {}

  /**
   * @brief Called when a new connection is established.
   *
   * Stores the socket fd and remote address, and initializes keep-alive state.
   *
   * @param fd     Accepted socket file descriptor.
   * @param remote Remote client address.
   */
  Task<void> on_connect(int fd, SocketAddr remote) override {
    fd_        = fd;
    remote_    = remote;
    keep_alive_ = true;
    co_return;
  }

  /**
   * @brief Called for each decoded HTTP request.
   *
   * ### Processing order
   * 1. Check `Upgrade: websocket` header → handle upgrade.
   * 2. Check `Expect: 100-continue` header → send `100 Continue`.
   * 3. Check `Connection` header → update keep-alive state.
   * 4. Dispatch request to Router.
   * 5. Serialize response and send over socket.
   *
   * @param frame Parsed HTTP request.
   * @returns Processing result. Returning an error closes the connection.
   */
  Task<Result<void>> on_frame(http::Request frame) override {
    // ── 1. Detect WebSocket upgrade ───────────────────────────────────────
    {
      std::string_view upgrade_hdr = frame.header("Upgrade");
      std::string upgrade_lower(upgrade_hdr);
      std::transform(upgrade_lower.begin(), upgrade_lower.end(),
                     upgrade_lower.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

      if (upgrade_lower == "websocket") {
        if (upgrade_callback_) {
          keep_alive_ = false; // Terminate HTTP keep-alive loop after upgrade
          co_await upgrade_callback_(UpgradeRequest{std::move(frame), fd_});
          co_return Result<void>{};
        } else {
          // Upgrade callback not registered — return 426 Upgrade Required
          static constexpr std::string_view kUpgradeRequired =
              "HTTP/1.1 426 Upgrade Required\r\n"
              "Connection: close\r\n"
              "Content-Length: 0\r\n"
              "\r\n";
          write_all(fd_, kUpgradeRequired);
          keep_alive_ = false;
          co_return Result<void>{};
        }
      }
    }

    // ── 2. Handle 100-continue ────────────────────────────────────────────
    {
      std::string_view expect_hdr = frame.header("Expect");
      if (expect_hdr == "100-continue") {
        static constexpr std::string_view k100Continue =
            "HTTP/1.1 100 Continue\r\n\r\n";
        if (!write_all(fd_, k100Continue)) {
          co_return unexpected(
              std::error_code(errno, std::system_category()));
        }
      }
    }

    // ── 3. Update keep-alive state from Connection header ─────────────────
    {
      std::string_view conn_hdr = frame.header("Connection");
      std::string conn_lower(conn_hdr);
      std::transform(conn_lower.begin(), conn_lower.end(),
                     conn_lower.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (conn_lower.find("close") != std::string::npos) {
        keep_alive_ = false;
      }
    }

    // ── 4. Router dispatch ────────────────────────────────────────────────
    Response res;

    if (router_) {
      std::unordered_map<std::string, std::string> params;
      auto handler_var = router_->match(frame.method(), frame.path(), params);

      // Inject matched parameters into the request
      for (auto &[k, v] : params) {
        frame.set_param(k, v);
      }

      if (std::holds_alternative<AsyncHandler>(handler_var)) {
        co_await std::get<AsyncHandler>(handler_var)(frame, res);
      } else if (std::holds_alternative<Handler>(handler_var)) {
        std::get<Handler>(handler_var)(frame, res);
      } else {
        // No route matched
        if (router_->path_exists(frame.path())) {
          res.status(405).header("Content-Length", "0");
        } else {
          res.status(404).body("Not Found");
        }
      }
    } else {
      res.status(503).body("No router configured");
    }

    // ── 5. Inject keep-alive response header ─────────────────────────────
    if (keep_alive_) {
      res.header("Connection", "keep-alive");
    } else {
      res.header("Connection", "close");
    }

    // ── 6. Serialize and send response (scatter-gather, zero-copy header+body) ─
    //
    // Instead of concatenating header and body into a single std::string,
    // we serialize only the header and send header + body in one ::writev(2)
    // syscall via a scattered_span. This avoids a heap allocation equal to
    // body_size bytes on every response.
    //
    // Fallback to write_all when the body is empty (no scatter needed) or
    // when a sendfile path is set (handled by the sendfile path below).
    {
      std::string_view body_sv = res.get_body();
      if (body_sv.empty()) {
        // Header-only response (e.g. 204 No Content, 304 Not Modified)
        std::string header_str = res.serialize_header();
        if (!write_all(fd_, header_str)) {
          co_return std::unexpected(std::error_code{errno, std::system_category()});
        }
      } else {
        // Header + body — two segments, one writev syscall, no extra allocation.
        std::string header_str = res.serialize_header();
        IOVec<2> vec;
        vec.push(header_str.data(), header_str.size());
        vec.push(body_sv.data(), body_sv.size());
        if (!writev_all(fd_, scattered_span{vec})) {
          co_return std::unexpected(std::error_code{errno, std::system_category()});
        }
      }
    }

    co_return Result<void>{};
  }

  /**
   * @brief Called when the connection is closed.
   *
   * Resets the socket fd. Calling `close()` is the responsibility of AcceptLoop.
   *
   * @param ec Error code indicating the reason for closure. Default (invalid) code for normal closure.
   */
  Task<void> on_disconnect(std::error_code /*ec*/) override {
    fd_ = -1;
    co_return;
  }

  /**
   * @brief Returns the current keep-alive state.
   *
   * Used by AcceptLoop or the transport layer to decide whether to reuse the connection.
   *
   * @returns true if keep-alive is active.
   */
  [[nodiscard]] bool keep_alive() const noexcept { return keep_alive_; }

private:
  /**
   * @brief Repeatedly writes to a socket until all data is sent.
   *
   * @param fd   Target socket file descriptor.
   * @param data Data to send.
   * @returns true on success, false on error.
   */
  static bool write_all(int fd, std::string_view data) noexcept {
    const char *ptr = data.data();
    size_t      remaining = data.size();
    while (remaining > 0) {
      ssize_t n = ::write(fd, ptr, remaining);
      if (n <= 0) return false;
      ptr       += static_cast<size_t>(n);
      remaining -= static_cast<size_t>(n);
    }
    return true;
  }

  /**
   * @brief Vectored write — sends all segments in a `scattered_span` via `::writev`.
   *
   * Handles short writes by advancing through the iovec array.  On each partial
   * write the leading fully-sent entries are skipped and the partially-sent
   * entry's base/len are adjusted (using stack-local copies — the original iovecs
   * are not mutated).
   *
   * @param fd      Target socket file descriptor.
   * @param scatter Scatter-gather list of read-only byte segments.
   * @returns true if all bytes were sent, false on write error.
   */
  static bool writev_all(int fd, scattered_span scatter) noexcept {
    if (scatter.empty()) return true;

    // Work on a stack copy of the iovecs so we can adjust base/len on short writes.
    // IOVec<64> covers virtually all realistic response segment counts.
    IOVec<64> working;
    for (const auto& iov : scatter.as_iovec()) {
      if (working.full()) break; // safety guard — segments beyond 64 are dropped
      working.push(iov.iov_base, iov.iov_len);
    }

    std::size_t idx = 0; // index of the first non-exhausted iovec
    while (idx < working.count) {
      ssize_t n = ::writev(fd,
                           working.vecs + idx,
                           static_cast<int>(working.count - idx));
      if (n <= 0) return false;

      // Advance past the bytes that were sent
      auto remaining = static_cast<std::size_t>(n);
      while (idx < working.count && remaining >= working.vecs[idx].iov_len) {
        remaining -= working.vecs[idx].iov_len;
        ++idx;
      }
      if (remaining > 0 && idx < working.count) {
        // Partial write within the current segment — adjust base and length
        auto* base = static_cast<std::byte*>(working.vecs[idx].iov_base);
        working.vecs[idx].iov_base = base + remaining;
        working.vecs[idx].iov_len -= remaining;
      }
    }
    return true;
  }

  /** @brief Router used to route requests. */
  std::shared_ptr<http::Router> router_;

  /** @brief WebSocket upgrade request callback. Upgrade is rejected if nullptr. */
  UpgradeCallback upgrade_callback_;

  /** @brief Socket file descriptor of the current connection. -1 when no connection. */
  int fd_{-1};

  /** @brief Remote client address. Set during on_connect(). */
  SocketAddr remote_{};

  /** @brief HTTP keep-alive active state. Connection is closed after response if false. */
  bool keep_alive_{true};
};

} // namespace qbuem

/** @} */ // end of qbuem_http1_handler
