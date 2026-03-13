#pragma once

#include <qbuem/core/reactor.hpp>

#include <functional>
#include <unordered_map>

namespace qbuem {

/**
 * @brief Linux-specific Reactor implementation using epoll + timerfd.
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

private:
  int epoll_fd_ = -1;
  bool running_ = true;
  int next_timer_id_ = 1;

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
