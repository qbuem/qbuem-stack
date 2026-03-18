#pragma once

#include <qbuem/core/reactor.hpp>

#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace qbuem {

/**
 * @brief Linux-specific Reactor implementation using epoll (edge-triggered) + timerfd.
 *
 * ## Edge-Triggered Mode (v2.4.0)
 * All I/O file descriptors are registered with `EPOLLET | EPOLLONESHOT`:
 * - `EPOLLET`: The kernel delivers exactly one event per readable/writable
 *   transition. Callbacks **must** drain data until `EAGAIN` to avoid
 *   missing subsequent data (no re-notification until new data arrives).
 * - `EPOLLONESHOT`: After each event fires, the fd is automatically disarmed.
 *   The reactor re-arms it with `EPOLL_CTL_MOD` after invoking the callback,
 *   ensuring at most one thread processes a given fd at a time (safe for
 *   multi-threaded `Dispatcher` configurations).
 *
 * ## Scalability
 * - Use `SO_REUSEPORT` on listening sockets to distribute new connections
 *   across multiple `EpollReactor` instances (one per CPU core) without a
 *   thundering-herd at `accept()`.
 *
 * Timers are implemented via timerfd_create(2) and registered as ordinary
 * epoll file-descriptor events, so the poll loop is a single epoll_wait call.
 */
class EpollReactor : public Reactor {
public:
  EpollReactor();
  ~EpollReactor() override;

  Result<void> register_event(int fd, EventType type,
                              std::function<void(int)> callback) override;

  Result<int> register_timer(int timeout_ms,
                             std::function<void(int)> callback) override;

  Result<void> unregister_event(int fd, EventType type) override;

  Result<void> unregister_timer(int timer_id) override;

  Result<int> poll(int timeout_ms) override;

  void stop() override;

  bool is_running() const override;

  void post(std::function<void()> fn) override;

private:
  int epoll_fd_ = -1;
  int event_fd_ = -1;   // eventfd used to wake epoll_wait from post()
  bool running_ = true;
  int next_timer_id_ = 1;

  std::mutex work_mutex_;
  std::vector<std::function<void()>> work_queue_;

  // Per-fd callbacks for regular I/O events
  struct FdCallbacks {
    std::function<void(int)> read_cb;
    std::function<void(int)> write_cb;
  };
  std::unordered_map<int, FdCallbacks> fd_callbacks_;

  // Timer state: timer_id -> {timerfd, callback}
  struct TimerEntry {
    int timerfd;
    std::function<void(int)> callback;
  };
  std::unordered_map<int, TimerEntry> timers_by_id_;
  std::unordered_map<int, int> timerfd_to_id_; // timerfd -> timer_id

  void update_epoll(int fd, bool add);
};

} // namespace qbuem
