#ifndef QBUEM_CORE_KQUEUE_REACTOR_HPP
#define QBUEM_CORE_KQUEUE_REACTOR_HPP

#include <qbuem/core/reactor.hpp>

#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace qbuem {

/**
 * @brief macOS specific Reactor implementation using kqueue.
 */
class KqueueReactor : public Reactor {
public:
  KqueueReactor();
  ~KqueueReactor() override;

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
  // Sentinel ident for the EVFILT_USER wake channel used by post().
  // EVFILT_USER is a separate filter namespace from EVFILT_READ/WRITE/TIMER,
  // so ident=0 here does not conflict with fd=0 in callbacks_.
  static constexpr uintptr_t WAKE_IDENT = 0;

  int kq_fd_ = -1;
  bool running_ = true;
  int next_timer_id_ = 1;

  std::mutex work_mutex_;
  std::vector<std::function<void()>> work_queue_;

  struct Callbacks {
    std::function<void(int)> read_cb;
    std::function<void(int)> write_cb;
    std::function<void(int)> timer_cb;
  };
  std::unordered_map<int, Callbacks> callbacks_;
};

} // namespace qbuem

#endif // QBUEM_CORE_KQUEUE_REACTOR_HPP
