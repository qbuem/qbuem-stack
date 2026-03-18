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
#include <qbuem/net/socket_addr.hpp>
#include <qbuem/server/connection_handler.hpp>

#include <algorithm>
#include <cerrno>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
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
   * @brief 디코딩된 HTTP 요청마다 호출됩니다.
   *
   * ### 처리 순서
   * 1. `Upgrade: websocket` 헤더 확인 → 업그레이드 처리.
   * 2. `Expect: 100-continue` 헤더 확인 → `100 Continue` 전송.
   * 3. `Connection` 헤더 확인 → keep-alive 상태 갱신.
   * 4. Router로 요청 디스패치.
   * 5. 응답 직렬화 후 소켓에 전송.
   *
   * @param frame 파싱된 HTTP 요청.
   * @returns 처리 결과. 에러 반환 시 연결이 종료됩니다.
   */
  Task<Result<void>> on_frame(http::Request frame) override {
    // ── 1. WebSocket 업그레이드 감지 ──────────────────────────────────────
    {
      std::string_view upgrade_hdr = frame.header("Upgrade");
      std::string upgrade_lower(upgrade_hdr);
      std::transform(upgrade_lower.begin(), upgrade_lower.end(),
                     upgrade_lower.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

      if (upgrade_lower == "websocket") {
        if (upgrade_callback_) {
          keep_alive_ = false; // 업그레이드 후 HTTP keep-alive 루프 종료
          co_await upgrade_callback_(UpgradeRequest{std::move(frame), fd_});
          co_return Result<void>::ok();
        } else {
          // 업그레이드 콜백 미등록 — 426 Upgrade Required 반환
          static constexpr std::string_view kUpgradeRequired =
              "HTTP/1.1 426 Upgrade Required\r\n"
              "Connection: close\r\n"
              "Content-Length: 0\r\n"
              "\r\n";
          write_all(fd_, kUpgradeRequired);
          keep_alive_ = false;
          co_return Result<void>::ok();
        }
      }
    }

    // ── 2. 100-continue 처리 ──────────────────────────────────────────────
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

    // ── 3. Connection 헤더로 keep-alive 상태 갱신 ─────────────────────────
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

    // ── 4. Router 디스패치 ────────────────────────────────────────────────
    Response res;

    if (router_) {
      std::unordered_map<std::string, std::string> params;
      auto handler_var = router_->match(frame.method(), frame.path(), params);

      // 매칭된 파라미터를 요청에 주입
      for (auto &[k, v] : params) {
        frame.set_param(k, v);
      }

      if (std::holds_alternative<AsyncHandler>(handler_var)) {
        co_await std::get<AsyncHandler>(handler_var)(frame, res);
      } else if (std::holds_alternative<Handler>(handler_var)) {
        std::get<Handler>(handler_var)(frame, res);
      } else {
        // 라우트 없음
        if (router_->path_exists(frame.path())) {
          res.status(405).header("Content-Length", "0");
        } else {
          res.status(404).body("Not Found");
        }
      }
    } else {
      res.status(503).body("No router configured");
    }

    // ── 5. keep-alive 응답 헤더 주입 ──────────────────────────────────────
    if (keep_alive_) {
      res.header("Connection", "keep-alive");
    } else {
      res.header("Connection", "close");
    }

    // ── 6. 응답 직렬화 및 전송 ────────────────────────────────────────────
    std::string serialized = res.serialize();
    if (!write_all(fd_, serialized)) {
      co_return unexpected(
          std::error_code(errno, std::system_category()));
    }

    co_return Result<void>::ok();
  }

  /**
   * @brief 연결이 종료될 때 호출됩니다.
   *
   * 소켓 fd를 초기화합니다. 실제 `close()` 호출은 AcceptLoop의 책임입니다.
   *
   * @param ec 종료 원인 에러 코드. 정상 종료면 기본값(무효) 코드.
   */
  Task<void> on_disconnect(std::error_code /*ec*/) override {
    fd_ = -1;
    co_return;
  }

  /**
   * @brief 현재 keep-alive 상태를 반환합니다.
   *
   * AcceptLoop 또는 전송 계층이 연결 재사용 여부를 결정할 때 사용합니다.
   *
   * @returns keep-alive가 활성 상태면 true.
   */
  [[nodiscard]] bool keep_alive() const noexcept { return keep_alive_; }

private:
  /**
   * @brief 소켓에 데이터를 모두 전송할 때까지 반복 write합니다.
   *
   * @param fd   대상 소켓 파일 디스크립터.
   * @param data 전송할 데이터.
   * @returns 성공 시 true, 에러 시 false.
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

  /** @brief 요청을 라우팅할 Router. */
  std::shared_ptr<http::Router> router_;

  /** @brief WebSocket 업그레이드 요청 콜백. nullptr이면 업그레이드 거부. */
  UpgradeCallback upgrade_callback_;

  /** @brief 현재 연결의 소켓 파일 디스크립터. -1이면 연결 없음. */
  int fd_{-1};

  /** @brief 원격 클라이언트 주소. on_connect() 시 설정됩니다. */
  SocketAddr remote_{};

  /** @brief HTTP keep-alive 활성 상태. false이면 응답 후 연결 종료. */
  bool keep_alive_{true};
};

} // namespace qbuem

/** @} */ // end of qbuem_http1_handler
