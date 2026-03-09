#include <draco/core/dispatcher.hpp>
#include <draco/core/io_uring_reactor.hpp>
#include <draco/core/kqueue_reactor.hpp>

#include <iostream>

namespace draco {

Dispatcher::Dispatcher(size_t thread_count) {
  for (size_t i = 0; i < thread_count; ++i) {
#ifdef __APPLE__
    reactors_.push_back(std::make_unique<KqueueReactor>());
#else
    reactors_.push_back(std::make_unique<IOUringReactor>());
#endif
  }
}

void Dispatcher::run() {
  running_ = true;
  std::vector<std::thread> threads;
  for (size_t i = 0; i < reactors_.size(); ++i) {
    threads.emplace_back([this, i]() {
      auto &reactor = reactors_[i];
      std::cout << "[Debug] Worker thread " << i << " starting..." << std::endl;
      Reactor::set_current(reactor.get());
      while (running_) {
        auto result = reactor->poll(100);
        if (!result || *result == 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
      std::cout << "[Debug] Worker thread " << i << " exiting." << std::endl;
    });
  }

  for (auto &t : threads) {
    if (t.joinable())
      t.join();
  }
}

void Dispatcher::stop() {
  running_ = false;
  for (auto &r : reactors_)
    r->stop();
}

Result<void> Dispatcher::register_listener(int fd,
                                           std::function<void(int)> callback) {
  if (reactors_.empty()) {
    return std::unexpected(std::make_error_code(std::errc::no_such_device));
  }
  return reactors_[0]->register_event(fd, EventType::Read, std::move(callback));
}

Reactor *Dispatcher::get_worker_reactor(int fd) {
  return reactors_[fd % reactors_.size()].get();
}

} // namespace draco
