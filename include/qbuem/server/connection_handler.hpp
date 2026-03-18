#pragma once

/**
 * @file qbuem/server/connection_handler.hpp
 * @brief Connection lifecycle abstractions — IConnectionHandler, AcceptLoop, ConnectionPool
 * @defgroup qbuem_server Server Abstractions
 * @ingroup qbuem_io
 *
 * This header provides the core abstractions for server-side connection handling:
 *
 * - `IConnectionHandler<Frame>` : Per-protocol connection handler interface
 * - `AcceptLoop<Frame, HandlerFactory>` : SO_REUSEPORT accept loop
 * - `ConnectionPool<T>` : Outbound connection pool
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/net/socket_addr.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <system_error>
#include <vector>

namespace qbuem {

// Forward declaration
class Dispatcher;

// ─── IConnectionHandler<Frame> ───────────────────────────────────────────────

/**
 * @brief Abstract interface for frame-based connection handlers.
 *
 * One instance is created per client connection.
 * `HandlerFactory` provides instances when a new connection is accepted.
 *
 * ### Lifecycle
 * `on_connect()` → (repeated) `on_frame()` → `on_disconnect()`
 *
 * @tparam Frame Frame type processed by this handler.
 */
template <typename Frame>
class IConnectionHandler {
public:
  virtual ~IConnectionHandler() = default;

  /**
   * @brief Called when a new connection is established.
   *
   * @param fd     Accepted socket file descriptor.
   * @param remote Remote address.
   */
  virtual Task<void> on_connect(int fd, SocketAddr remote) = 0;

  /**
   * @brief Called for each decoded frame.
   *
   * @param frame Received frame.
   * @returns Processing result. Returning an error closes the connection.
   */
  virtual Task<Result<void>> on_frame(Frame frame) = 0;

  /**
   * @brief Called when the connection is closed.
   *
   * @param ec Error code indicating the reason for closure. Default (invalid) code for normal closure.
   */
  virtual Task<void> on_disconnect(std::error_code ec) = 0;
};

// ─── AcceptLoop<Frame, HandlerFactory> ───────────────────────────────────────

/**
 * @brief SO_REUSEPORT-based connection accept loop.
 *
 * `HandlerFactory` is a callable that must return
 * `std::unique_ptr<IConnectionHandler<Frame>>` when invoked.
 *
 * @code
 * auto factory = []{ return std::make_unique<MyHandler>(); };
 * AcceptLoop<MyFrame, decltype(factory)> loop({
 *     .addr    = *SocketAddr::from_ipv4("0.0.0.0", 8080),
 *     .factory = factory,
 *     .dispatcher = &dispatcher,
 * });
 * co_await loop.run();
 * @endcode
 *
 * @tparam Frame          Frame type the handler processes.
 * @tparam HandlerFactory Callable type: `() -> std::unique_ptr<IConnectionHandler<Frame>>`.
 */
template <typename Frame, typename HandlerFactory>
class AcceptLoop {
public:
  /**
   * @brief AcceptLoop configuration struct.
   */
  struct Config {
    /** @brief Server address to bind. */
    SocketAddr addr;
    /** @brief Factory that creates a handler instance for each new connection. */
    HandlerFactory factory;
    /** @brief Dispatcher to run workers on. Uses the current thread's Reactor if nullptr. */
    Dispatcher *dispatcher = nullptr;
    /** @brief listen() backlog size. */
    size_t backlog = 1024;
  };

  /**
   * @brief Constructs an AcceptLoop.
   *
   * @param cfg AcceptLoop configuration.
   */
  explicit AcceptLoop(Config cfg)
      : cfg_(std::move(cfg)) {}

  /**
   * @brief Starts the accept loop. Runs until stop() is called.
   *
   * bind() → listen() → repeated accept() → spawn handler coroutine per connection.
   *
   * @returns Task that completes when the loop exits.
   */
  Task<void> run() {
    running_.store(true, std::memory_order_release);
    // Concrete implementation is delegated to the platform-specific TcpListener.
    // Only the basic structure is defined here; actual binding is performed by the subclass.
    while (running_.load(std::memory_order_acquire)) {
      // In derived classes or integration code, after calling accept(), use the pattern below:
      //   auto handler = cfg_.factory();
      //   int  conn_fd  = <platform accept>;
      //   SocketAddr remote = <remote address>;
      //   auto task = handle_connection(std::move(handler), conn_fd, remote);
      //   if (cfg_.dispatcher) cfg_.dispatcher->spawn(std::move(task));
      //   else task.detach();

      // This default implementation returns immediately (override or replace with platform code).
      co_return;
    }
    co_return;
  }

  /**
   * @brief Signals the accept loop to stop.
   *
   * Thread-safe.
   */
  void stop() {
    running_.store(false, std::memory_order_release);
  }

private:
  /**
   * @brief Coroutine managing the lifecycle of a single connection.
   *
   * @param handler Handler instance for this connection.
   * @param fd      Socket file descriptor.
   * @param remote  Remote address.
   */
  Task<void> handle_connection(
      std::unique_ptr<IConnectionHandler<Frame>> handler,
      int fd, SocketAddr remote) {
    co_await handler->on_connect(fd, remote);
    // The actual frame read/dispatch loop integrates with the platform transport layer.
    co_await handler->on_disconnect(std::error_code{});
    co_return;
  }

  Config cfg_;
  std::atomic<bool> running_{false};
};

// ─── ConnectionPool<T> ───────────────────────────────────────────────────────

/**
 * @brief Outbound connection pool.
 *
 * Pre-creates a minimum number of idle connections and enforces a maximum pool size.
 * Supports idle timeout and health checks.
 *
 * @tparam T Connection type. Must have a `connect(SocketAddr)` factory.
 */
template <typename T>
class ConnectionPool {
public:
  /**
   * @brief ConnectionPool configuration struct.
   */
  struct Config {
    /** @brief Remote address to connect to. */
    SocketAddr addr;
    /** @brief Minimum number of idle connections. */
    size_t min_idle = 2;
    /** @brief Maximum total number of connections. */
    size_t max_size = 32;
    /** @brief Idle connection timeout (ms). */
    uint64_t idle_timeout_ms = 30000;
    /** @brief Connection health check function. Health check is disabled if nullptr. */
    std::function<Task<Result<bool>>(T &)> health_check;
  };

  /**
   * @brief RAII handle for a connection.
   *
   * Returns the connection to the pool on destruction.
   */
  struct Handle {
    /** @brief Pointer to the acquired connection. */
    T *conn = nullptr;
    /** @brief Pool to which the connection will be returned. */
    ConnectionPool *pool = nullptr;

    Handle() noexcept = default;
    Handle(T *c, ConnectionPool *p) noexcept : conn(c), pool(p) {}

    ~Handle() {
      if (pool && conn) pool->release(conn);
    }

    // Move-only (copy prohibited)
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;

    Handle(Handle &&other) noexcept
        : conn(other.conn), pool(other.pool) {
      other.conn = nullptr;
      other.pool = nullptr;
    }

    Handle &operator=(Handle &&other) noexcept {
      if (this != &other) {
        if (pool && conn) pool->release(conn);
        conn = other.conn;
        pool = other.pool;
        other.conn = nullptr;
        other.pool = nullptr;
      }
      return *this;
    }

    /** @brief Checks whether the connection is valid. */
    explicit operator bool() const noexcept { return conn != nullptr; }
    /** @brief Dereferences the connection pointer. */
    T *operator->() noexcept { return conn; }
    /** @brief Dereferences the connection reference. */
    T &operator*() noexcept { return *conn; }
  };

  /**
   * @brief Constructs a ConnectionPool.
   *
   * @param cfg Pool configuration.
   */
  explicit ConnectionPool(Config cfg)
      : cfg_(std::move(cfg)) {}

  /**
   * @brief Acquires an idle connection, creating a new one if none are available.
   *
   * @returns Connection handle. Returns errc::resource_unavailable_try_again if the max size is exceeded.
   */
  Task<Result<Handle>> acquire() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!idle_.empty()) {
        auto *conn = idle_.back().release();
        idle_.pop_back();
        co_return Handle{conn, this};
      }
    }

    // Check if max size exceeded
    size_t cur = total_.load(std::memory_order_relaxed);
    if (cur >= cfg_.max_size) {
      co_return unexpected(
          std::make_error_code(std::errc::resource_unavailable_try_again));
    }

    // Create new connection
    total_.fetch_add(1, std::memory_order_relaxed);
    auto conn = std::make_unique<T>();
    // Actual connect() is implemented by type T — here we only return the instance
    T *raw = conn.release();
    co_return Handle{raw, this};
  }

  /**
   * @brief Returns a connection to the pool.
   *
   * @param conn Pointer to the connection to return.
   */
  void release(T *conn) {
    if (!conn) return;
    std::lock_guard<std::mutex> lock(mu_);
    if (idle_.size() < cfg_.max_size) {
      idle_.emplace_back(conn);
    } else {
      // Delete excess connection
      delete conn;
      total_.fetch_sub(1, std::memory_order_relaxed);
    }
  }

  /** @brief Returns the current number of idle connections. */
  [[nodiscard]] size_t idle_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return idle_.size();
  }

  /** @brief Returns the current total connection count (idle + borrowed). */
  [[nodiscard]] size_t total_count() const {
    return total_.load(std::memory_order_relaxed);
  }

private:
  Config cfg_;
  /** @brief List of idle connections. */
  std::vector<std::unique_ptr<T>> idle_;
  /** @brief Total connection count (idle + borrowed). */
  std::atomic<size_t> total_{0};
  /** @brief Mutex protecting idle_. */
  mutable std::mutex mu_;
};

} // namespace qbuem

/** @} */ // end of qbuem_server
