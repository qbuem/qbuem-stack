#ifndef DRACO_CORE_KQUEUE_REACTOR_HPP
#define DRACO_CORE_KQUEUE_REACTOR_HPP

#include <draco/core/reactor.hpp>

#include <functional>
#include <unordered_map>

namespace draco {

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

private:
  int kq_fd_ = -1;
  bool running_ = true;
  int next_timer_id_ = 1;

  struct Callbacks {
    std::function<void(int)> read_cb;
    std::function<void(int)> write_cb;
    std::function<void(int)> timer_cb;
  };
  std::unordered_map<int, Callbacks> callbacks_;
};

} // namespace draco

#endif // DRACO_CORE_KQUEUE_REACTOR_HPP
