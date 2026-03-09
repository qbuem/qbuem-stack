#pragma once

#include <draco/core/reactor.hpp>

/**
 * @brief Placeholder for Linux io_uring Reactor.
 *
 * This will be implemented using the liburing library or direct syscalls.
 */
namespace draco {

class IOUringReactor final : public Reactor {
public:
  IOUringReactor() {
    // io_uring_queue_init(...)
  }

  ~IOUringReactor() override {
    // io_uring_queue_exit(...)
  }

  Result<void> register_event(int, EventType,
                              std::function<void(int)>) override {
    // SQE submission
    return {};
  }

  Result<int> register_timer(int, std::function<void(int)>) override {
    return 0;
  }

  Result<void> unregister_event(int, EventType) override { return {}; }

  Result<void> unregister_timer(int) override { return {}; }

  Result<int> poll(int) override {
    // io_uring_wait_cqe(...)
    return 0;
  }

  void stop() override { running_ = false; }
  bool is_running() const override { return running_; }

private:
  bool running_ = false;
  // struct io_uring ring_;
};

} // namespace draco
