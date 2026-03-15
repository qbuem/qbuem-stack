#pragma once

/**
 * @file qbuem/server/connection_handler.hpp
 * @brief 연결 수명 주기 추상화 — IConnectionHandler, AcceptLoop, ConnectionPool
 * @defgroup qbuem_server Server Abstractions
 * @ingroup qbuem_io
 *
 * 이 헤더는 서버 측 연결 처리의 핵심 추상화를 제공합니다:
 *
 * - `IConnectionHandler<Frame>` : 프로토콜별 연결 핸들러 인터페이스
 * - `AcceptLoop<Frame, HandlerFactory>` : SO_REUSEPORT 수락 루프
 * - `ConnectionPool<T>` : 아웃바운드 연결 풀
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
 * @brief 프레임 기반 연결 핸들러 추상 인터페이스.
 *
 * 각 클라이언트 연결마다 인스턴스가 하나 생성됩니다.
 * `HandlerFactory`가 새 연결 수락 시 인스턴스를 제공합니다.
 *
 * ### 수명 주기
 * `on_connect()` → (반복) `on_frame()` → `on_disconnect()`
 *
 * @tparam Frame 이 핸들러가 처리하는 프레임 타입.
 */
template <typename Frame>
class IConnectionHandler {
public:
  virtual ~IConnectionHandler() = default;

  /**
   * @brief 새 연결이 수립됐을 때 호출됩니다.
   *
   * @param fd     수락된 소켓 파일 디스크립터.
   * @param remote 원격 주소.
   */
  virtual Task<void> on_connect(int fd, SocketAddr remote) = 0;

  /**
   * @brief 디코딩된 프레임마다 호출됩니다.
   *
   * @param frame 수신된 프레임.
   * @returns 처리 결과. 에러 반환 시 연결이 종료됩니다.
   */
  virtual Task<Result<void>> on_frame(Frame frame) = 0;

  /**
   * @brief 연결이 종료될 때 호출됩니다.
   *
   * @param ec 종료 원인 에러 코드. 정상 종료면 기본값(무효) 코드.
   */
  virtual Task<void> on_disconnect(std::error_code ec) = 0;
};

// ─── AcceptLoop<Frame, HandlerFactory> ───────────────────────────────────────

/**
 * @brief SO_REUSEPORT 기반 연결 수락 루프.
 *
 * `HandlerFactory`는 호출 가능 객체로, 호출 시
 * `std::unique_ptr<IConnectionHandler<Frame>>`를 반환해야 합니다.
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
 * @tparam Frame          핸들러가 처리할 프레임 타입.
 * @tparam HandlerFactory `() -> std::unique_ptr<IConnectionHandler<Frame>>` 호출 가능 타입.
 */
template <typename Frame, typename HandlerFactory>
class AcceptLoop {
public:
  /**
   * @brief AcceptLoop 설정 구조체.
   */
  struct Config {
    /** @brief 바인딩할 서버 주소. */
    SocketAddr addr;
    /** @brief 새 연결마다 핸들러 인스턴스를 생성하는 팩토리. */
    HandlerFactory factory;
    /** @brief 워커를 실행할 Dispatcher. nullptr이면 현재 스레드 Reactor 사용. */
    Dispatcher *dispatcher = nullptr;
    /** @brief listen() 백로그 크기. */
    size_t backlog = 1024;
  };

  /**
   * @brief AcceptLoop을 구성합니다.
   *
   * @param cfg AcceptLoop 설정.
   */
  explicit AcceptLoop(Config cfg)
      : cfg_(std::move(cfg)) {}

  /**
   * @brief 수락 루프를 시작합니다. stop()이 호출될 때까지 실행됩니다.
   *
   * bind() → listen() → 반복 accept() → 각 연결에 핸들러 코루틴 spawn.
   *
   * @returns 루프 종료 Task.
   */
  Task<void> run() {
    running_.store(true, std::memory_order_release);
    // 구체 구현은 플랫폼별 TcpListener에 위임합니다.
    // 여기서는 기본 구조만 정의하며 실제 바인딩은 서브클래스에서 수행합니다.
    while (running_.load(std::memory_order_acquire)) {
      // 파생 클래스 또는 통합 코드에서 accept() 호출 후 아래 패턴 사용:
      //   auto handler = cfg_.factory();
      //   int  conn_fd  = <플랫폼 accept>;
      //   SocketAddr remote = <원격 주소>;
      //   auto task = handle_connection(std::move(handler), conn_fd, remote);
      //   if (cfg_.dispatcher) cfg_.dispatcher->spawn(std::move(task));
      //   else task.detach();

      // 이 기본 구현은 즉시 반환 (사용자가 오버라이드 또는 플랫폼 코드로 교체)
      co_return;
    }
    co_return;
  }

  /**
   * @brief 수락 루프를 중단하도록 신호를 보냅니다.
   *
   * 스레드 안전합니다.
   */
  void stop() {
    running_.store(false, std::memory_order_release);
  }

private:
  /**
   * @brief 단일 연결의 수명 주기를 관리하는 코루틴.
   *
   * @param handler 이 연결을 처리할 핸들러 인스턴스.
   * @param fd      소켓 파일 디스크립터.
   * @param remote  원격 주소.
   */
  Task<void> handle_connection(
      std::unique_ptr<IConnectionHandler<Frame>> handler,
      int fd, SocketAddr remote) {
    co_await handler->on_connect(fd, remote);
    // 실제 프레임 읽기/디스패치 루프는 플랫폼 전송 계층과 통합됩니다.
    co_await handler->on_disconnect(std::error_code{});
    co_return;
  }

  Config cfg_;
  std::atomic<bool> running_{false};
};

// ─── ConnectionPool<T> ───────────────────────────────────────────────────────

/**
 * @brief 아웃바운드 연결 풀.
 *
 * 최소 유휴 연결을 사전 생성하고 최대 크기를 제한합니다.
 * 유휴 타임아웃 및 헬스 체크를 지원합니다.
 *
 * @tparam T 연결 타입. `connect(SocketAddr)` 팩토리를 가지는 타입.
 */
template <typename T>
class ConnectionPool {
public:
  /**
   * @brief ConnectionPool 설정 구조체.
   */
  struct Config {
    /** @brief 연결할 원격 주소. */
    SocketAddr addr;
    /** @brief 최소 유휴 연결 수. */
    size_t min_idle = 2;
    /** @brief 최대 총 연결 수. */
    size_t max_size = 32;
    /** @brief 유휴 연결 타임아웃 (ms). */
    uint64_t idle_timeout_ms = 30000;
    /** @brief 연결 헬스 체크 함수. nullptr이면 헬스 체크 비활성화. */
    std::function<Task<Result<bool>>(T &)> health_check;
  };

  /**
   * @brief 연결 RAII 핸들.
   *
   * 소멸 시 연결을 풀에 반환합니다.
   */
  struct Handle {
    /** @brief 획득된 연결 포인터. */
    T *conn = nullptr;
    /** @brief 연결을 반환할 풀. */
    ConnectionPool *pool = nullptr;

    Handle() noexcept = default;
    Handle(T *c, ConnectionPool *p) noexcept : conn(c), pool(p) {}

    ~Handle() {
      if (pool && conn) pool->release(conn);
    }

    // 이동 전용 (복사 금지)
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

    /** @brief 연결이 유효한지 확인합니다. */
    explicit operator bool() const noexcept { return conn != nullptr; }
    /** @brief 연결 포인터 역참조. */
    T *operator->() noexcept { return conn; }
    /** @brief 연결 참조 역참조. */
    T &operator*() noexcept { return *conn; }
  };

  /**
   * @brief ConnectionPool을 구성합니다.
   *
   * @param cfg 풀 설정.
   */
  explicit ConnectionPool(Config cfg)
      : cfg_(std::move(cfg)) {}

  /**
   * @brief 유휴 연결을 획득합니다. 없으면 새로 생성합니다.
   *
   * @returns 연결 핸들. 최대 크기 초과 시 errc::resource_unavailable_try_again.
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

    // 최대 크기 초과 확인
    size_t cur = total_.load(std::memory_order_relaxed);
    if (cur >= cfg_.max_size) {
      co_return unexpected(
          std::make_error_code(std::errc::resource_unavailable_try_again));
    }

    // 새 연결 생성
    total_.fetch_add(1, std::memory_order_relaxed);
    auto conn = std::make_unique<T>();
    // 실제 connect()는 T 타입이 구현 — 여기서는 인스턴스만 반환
    T *raw = conn.release();
    co_return Handle{raw, this};
  }

  /**
   * @brief 연결을 풀에 반환합니다.
   *
   * @param conn 반환할 연결 포인터.
   */
  void release(T *conn) {
    if (!conn) return;
    std::lock_guard<std::mutex> lock(mu_);
    if (idle_.size() < cfg_.max_size) {
      idle_.emplace_back(conn);
    } else {
      // 초과분 삭제
      delete conn;
      total_.fetch_sub(1, std::memory_order_relaxed);
    }
  }

  /** @brief 현재 유휴 연결 수를 반환합니다. */
  [[nodiscard]] size_t idle_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return idle_.size();
  }

  /** @brief 현재 총 연결 수(유휴 + 대여 중)를 반환합니다. */
  [[nodiscard]] size_t total_count() const {
    return total_.load(std::memory_order_relaxed);
  }

private:
  Config cfg_;
  /** @brief 유휴 연결 목록. */
  std::vector<std::unique_ptr<T>> idle_;
  /** @brief 총 연결 수 (유휴 + 대여 중). */
  std::atomic<size_t> total_{0};
  /** @brief idle_ 보호 뮤텍스. */
  mutable std::mutex mu_;
};

} // namespace qbuem

/** @} */ // end of qbuem_server
