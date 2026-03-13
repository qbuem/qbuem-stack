#pragma once

#include <draco/common.hpp>
#include <draco/core/dispatcher.hpp>
#include <draco/http/router.hpp>
#include <draco/version.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string_view>
#include <thread>

namespace draco {

/**
 * @brief Snapshot of application metrics.
 *
 * All counters are atomic; a snapshot is consistent per-field but not
 * globally atomic across fields.
 */
struct Metrics {
  uint64_t requests_total     = 0; ///< Total HTTP requests handled.
  uint64_t errors_total       = 0; ///< Requests that resulted in 4xx/5xx.
  uint64_t active_connections = 0; ///< Currently open connections.
  uint64_t bytes_sent         = 0; ///< Total response bytes written to sockets.
};

/**
 * @brief Main application class for Draco WAS.
 */
class App {
public:
  /**
   * @brief Construct a new App object.
   * @param thread_count Number of reactor threads. Defaults to hardware
   * concurrency.
   */
  explicit App(size_t thread_count = std::thread::hardware_concurrency());

  /** @brief Register a global middleware. */
  void use(Middleware mw);

  /**
   * @brief Register a global error handler.
   *
   * Called when a sync handler throws an exception.  If not set, a generic
   * 500 response is sent.  Only one error handler can be registered; the
   * last call to on_error() wins.
   *
   * Example:
   *   app.on_error([](std::exception_ptr ep, const draco::Request& req,
   *                   draco::Response& res) {
   *     try { std::rethrow_exception(ep); }
   *     catch (const std::runtime_error& e) {
   *       res.status(500)
   *          .header("Content-Type", "application/json")
   *          .body(std::string("{\"error\":\"") + e.what() + "\"}");
   *     }
   *   });
   */
  void on_error(ErrorHandler handler);

  /** @brief Register a GET route. */
  void get(std::string_view path, HandlerVariant handler);

  /** @brief Register a POST route. */
  void post(std::string_view path, HandlerVariant handler);

  /** @brief Register a PUT route. */
  void put(std::string_view path, HandlerVariant handler);

  /**
   * @brief Register a DELETE route.
   * Named del() to avoid collision with the C keyword \c delete.
   */
  void del(std::string_view path, HandlerVariant handler);

  /** @brief Register a PATCH route. */
  void patch(std::string_view path, HandlerVariant handler);

  /**
   * @brief Register a HEAD route.
   *
   * If no explicit HEAD handler is registered but a GET handler exists for the
   * same path, Draco automatically serves HEAD using the GET handler and strips
   * the response body before sending.
   */
  void head(std::string_view path, HandlerVariant handler);

  /**
   * @brief Register an OPTIONS route (CORS preflight etc.).
   */
  void options(std::string_view path, HandlerVariant handler);

  /**
   * @brief Serve static files from a filesystem directory.
   *
   * Registers a GET handler that serves all files under @p url_prefix by
   * mapping the request path suffix to a file inside @p root_dir.
   *
   * Features:
   *   - MIME type auto-detection from file extension
   *   - Weak ETag (size + mtime) + Last-Modified headers → 304 support
   *   - Path traversal prevention (/../, %2e%2e, …)
   *
   * Example:
   *   app.serve_static("/static", "./public");
   *   // GET /static/js/app.js → reads ./public/js/app.js
   *
   * @param url_prefix  URL path prefix, e.g., "/static".
   * @param root_dir    Filesystem root directory, e.g., "./public".
   */
  void serve_static(std::string_view url_prefix, std::string_view root_dir);

  /**
   * @brief Register a built-in health check endpoint.
   *
   * Responds with HTTP 200 and `{"status":"ok"}` (Content-Type: application/json).
   * Defaults to GET /health if no path is given.
   *
   * Example: app.health_check();           // GET /health
   *          app.health_check("/_ping");    // GET /_ping
   */
  void health_check(std::string_view path = "/health");

  /**
   * @brief Register a detailed health check endpoint.
   *
   * Returns JSON with active connections and uptime:
   *   {"status":"ok","connections":N,"uptime_s":T,"requests_total":N}
   *
   * @param path  URL path (default: "/health/detail").
   */
  void health_check_detailed(std::string_view path = "/health/detail");

  /**
   * @brief Register a Kubernetes liveness probe endpoint.
   *
   * Returns 200 while the process is alive (always succeeds unless killed).
   * Kubernetes uses this to decide whether to restart the container.
   *
   * @param path  URL path (default: "/live").
   */
  void liveness_endpoint(std::string_view path = "/live");

  /**
   * @brief Register a Kubernetes readiness probe endpoint.
   *
   * Returns 200 when the server is ready to accept traffic, 503 during drain.
   * Kubernetes stops routing traffic to the pod when this returns non-2xx.
   *
   * @param path  URL path (default: "/ready").
   */
  void readiness_endpoint(std::string_view path = "/ready");

  /**
   * @brief Set a custom access-log callback.
   *
   * Called after each response is sent (from the reactor thread — keep it fast
   * or hand off to a ring buffer).
   *
   * Parameters: method, path, HTTP status code, duration in microseconds.
   *
   * Example:
   *   app.set_access_logger([](std::string_view m, std::string_view p,
   *                            int s, long us) {
   *     fprintf(stderr, "%s %s %d %ld µs\n", m.data(), p.data(), s, us);
   *   });
   */
  void set_access_logger(
      std::function<void(std::string_view method, std::string_view path,
                         int status, long duration_us)> fn);

  /**
   * @brief Enable the built-in access log (Apache Combined-Log-like format).
   *
   * Writes one line per request to stderr:
   *   [ISO8601] METHOD /path STATUS Xµs
   */
  void enable_access_log();

  /**
   * @brief Register a GET /metrics endpoint returning a text snapshot.
   *
   * Returns Prometheus-compatible plain-text exposition:
   *   draco_requests_total N
   *   draco_errors_total N
   *   draco_active_connections N
   *   draco_bytes_sent N
   *
   * @param path  URL path (default: "/metrics").
   */
  void metrics_endpoint(std::string_view path = "/metrics");

  /** @brief Return a snapshot of current application metrics. */
  Metrics snapshot_metrics() const;

  /**
   * @brief Start listening on a port (blocks until stop() is called).
   *
   * SIGTERM and SIGINT are handled automatically: they trigger a graceful
   * drain — no new connections are accepted, and the server exits after the
   * current poll cycle finishes (≤100 ms).
   *
   * @param port   TCP port to bind.
   * @param ipv6   If true, listen on IPv6 (dual-stack when IPV6_V6ONLY=0).
   *               Default: false (IPv4 only).
   */
  Result<void> listen(int port, bool ipv6 = false);

  /**
   * @brief Listen on a Unix Domain Socket path (AF_UNIX SOCK_STREAM).
   *
   * Useful for local IPC (reverse proxy, containers) without TCP overhead.
   * The socket file is removed on graceful shutdown.
   *
   * Example:
   *   app.listen_unix("/tmp/myapp.sock");
   *
   * @param path  Filesystem path for the Unix socket (max ~104 chars, OS limit).
   */
  Result<void> listen_unix(std::string_view path);

  /**
   * @brief Request graceful shutdown.
   *
   * Safe to call from a signal handler (async-signal-safe: sets an atomic).
   * listen() will return after the next reactor poll cycle.
   */
  void stop();

  // ── Internal atomic counters (updated by reactor threads) ────────────────
  // Placed on separate 64-byte cache lines to prevent false sharing across cores.
  static constexpr size_t kCacheLine = 64;
  alignas(kCacheLine) std::atomic<uint64_t> cnt_requests_{0};
  alignas(kCacheLine) std::atomic<uint64_t> cnt_errors_{0};
  alignas(kCacheLine) std::atomic<uint64_t> cnt_active_{0};
  alignas(kCacheLine) std::atomic<uint64_t> cnt_bytes_sent_{0};

private:
  Dispatcher dispatcher_;
  Router     router_;

  // Access logger callback (null = disabled).
  std::function<void(std::string_view, std::string_view, int, long)> logger_;

  // Error handler callback (null = default 500 response).
  ErrorHandler error_handler_;

  // Drain flag: set on SIGTERM/SIGINT; readiness probe returns 503 when true.
  std::atomic<bool> draining_{false};

  // Server start time (set on listen()/listen_unix()).
  std::chrono::steady_clock::time_point start_time_{std::chrono::steady_clock::now()};
};

// ─── StackController ─────────────────────────────────────────────────────────
/**
 * @brief Global lifecycle controller for the Draco WAS framework.
 *
 * StackController owns one or more App instances and provides centralized
 * startup, graceful shutdown, and signal handling.
 *
 * Usage:
 *   StackController ctrl;
 *   ctrl.add(app1, 8080);
 *   ctrl.add(app2, 8081);
 *   ctrl.run();           // blocks; handles SIGTERM/SIGINT automatically
 */
class StackController {
public:
  StackController() = default;
  ~StackController() = default;

  StackController(const StackController &) = delete;
  StackController &operator=(const StackController &) = delete;

  /**
   * @brief Register an App to listen on a port.
   *
   * @param app   Reference to the App (must outlive StackController::run()).
   * @param port  TCP port to bind.
   * @param ipv6  Enable IPv6 dual-stack.
   */
  void add(App &app, int port, bool ipv6 = false);

  /**
   * @brief Start all registered apps and block until all are stopped.
   *
   * Installs SIGTERM/SIGINT handlers.  Each app listens on its configured
   * port in its own thread; run() joins all threads before returning.
   */
  void run();

  /**
   * @brief Request graceful shutdown of all apps.
   *
   * Safe to call from a signal handler or another thread.
   */
  void stop();

private:
  struct Entry {
    App *app;
    int  port;
    bool ipv6;
  };

  std::vector<Entry>      entries_;
  std::atomic<bool>       stopping_{false};
};

} // namespace draco
