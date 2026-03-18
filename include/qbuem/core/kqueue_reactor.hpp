#ifndef QBUEM_CORE_KQUEUE_REACTOR_HPP
#define QBUEM_CORE_KQUEUE_REACTOR_HPP

#include <qbuem/core/reactor.hpp>
#include <qbuem/core/arena.hpp>
#include <qbuem/core/timer_wheel.hpp>

#include <functional>
#include <mutex>
#include <vector>
#include <sys/event.h>

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

  Result<void> register_signal(int sig, std::function<void(int)> callback) override;
  Result<void> unregister_signal(int sig) override;

  Result<void> unregister_timer(int timer_id) override;

  Result<int> poll(int timeout_ms) override;

  void stop() override;

  bool is_running() const override;

  void post(std::function<void()> fn) override;

private:
  struct alignas(64) KqueueEntry {
    int ident;
    std::function<void(int)> read_cb;
    std::function<void(int)> write_cb;
    std::function<void(int)> signal_cb;
    bool active = false;
  };

  void flush_changes();
  KqueueEntry* get_or_create_entry(int fd);

  static constexpr uintptr_t WAKE_IDENT = 0;

  int kq_fd_ = -1;
  bool running_ = true;

  std::mutex work_mutex_;
  std::vector<std::function<void()>> work_queue_;

  // Batching & Direct Dispatch
  std::vector<struct kevent> changelist_;
  std::vector<struct kevent> events_;

  // Zero-allocation pool and fast lookup
  FixedPoolResource<sizeof(KqueueEntry), alignof(KqueueEntry)> entry_pool_;
  std::vector<KqueueEntry*> entry_map_;

  // High-performance timer management
  TimerWheel timer_wheel_;
  uint64_t last_tick_ms_ = 0;
};

} // namespace qbuem

#endif // QBUEM_CORE_KQUEUE_REACTOR_HPP
