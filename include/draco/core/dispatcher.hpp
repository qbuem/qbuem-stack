#pragma once

#include <draco/common.hpp>
#include <draco/core/reactor.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace draco {

/**
 * @brief Dispatcher manages an array of Reactors, one per CPU core.
 */
class Dispatcher {
public:
  explicit Dispatcher(
      size_t thread_count = std::thread::hardware_concurrency());

  void run();
  void stop();

  Result<void> register_listener(int fd, std::function<void(int)> callback);
  Reactor *get_worker_reactor(int fd);

private:
  std::atomic<bool> running_{false};
  std::vector<std::unique_ptr<Reactor>> reactors_;
};

} // namespace draco
