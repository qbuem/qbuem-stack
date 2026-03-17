#include <qbuem/core/dispatcher.hpp>

#ifdef __APPLE__
#include <qbuem/core/kqueue_reactor.hpp>
#elif defined(QBUEM_HAS_IOURING)
#include <qbuem/core/io_uring_reactor.hpp>
#else
#include <qbuem/core/epoll_reactor.hpp>
#endif

namespace qbuem {

Dispatcher::Dispatcher(size_t thread_count) {
  for (size_t i = 0; i < thread_count; ++i) {
#ifdef __APPLE__
    reactors_.push_back(std::make_unique<KqueueReactor>());
#elif defined(QBUEM_HAS_IOURING)
    reactors_.push_back(std::make_unique<IOUringReactor>());
#else
    reactors_.push_back(std::make_unique<EpollReactor>());
#endif
  }
}

void Dispatcher::run() {
  running_ = true;
  std::vector<std::jthread> threads;
  for (size_t i = 0; i < reactors_.size(); ++i) {
    threads.emplace_back([this, i]() {
      auto &reactor = reactors_[i];
      Reactor::set_current(reactor.get());
      while (running_) {
        // poll() blocks up to 100 ms internally (epoll_wait / io_uring_wait).
        // No additional sleep needed — it would only add latency.
        reactor->poll(100);
      }
    });
  }
  // std::jthread auto-joins on destruction when threads goes out of scope.
}

void Dispatcher::stop() {
  running_ = false;
  for (auto &r : reactors_)
    r->stop();
}

Result<void> Dispatcher::register_listener(int fd,
                                           std::function<void(int)> callback) {
  if (reactors_.empty()) {
    return unexpected(std::make_error_code(std::errc::no_such_device));
  }
  return reactors_[0]->register_event(fd, EventType::Read, std::move(callback));
}

Result<void> Dispatcher::register_listener_at(int fd, size_t reactor_idx,
                                               std::function<void(int)> callback) {
  if (reactor_idx >= reactors_.size()) {
    return unexpected(std::make_error_code(std::errc::invalid_argument));
  }
  return reactors_[reactor_idx]->register_event(fd, EventType::Read,
                                                 std::move(callback));
}

Reactor *Dispatcher::get_worker_reactor(int fd) {
  return reactors_[fd % reactors_.size()].get();
}

void Dispatcher::post(std::function<void()> fn) {
  if (reactors_.empty())
    return;
  size_t idx = next_post_idx_.fetch_add(1, std::memory_order_relaxed) %
               reactors_.size();
  reactors_[idx]->post(std::move(fn));
}

void Dispatcher::post_to(size_t reactor_idx, std::function<void()> fn) {
  if (reactor_idx < reactors_.size())
    reactors_[reactor_idx]->post(std::move(fn));
}

void Dispatcher::spawn(Task<void> task) {
  if (reactors_.empty())
    return;
  auto h = task.handle;
  task.detach();
  size_t idx = next_post_idx_.fetch_add(1, std::memory_order_relaxed) %
               reactors_.size();
  reactors_[idx]->post([h]() mutable { h.resume(); });
}

void Dispatcher::spawn_on(size_t reactor_idx, Task<void> task) {
  if (reactor_idx >= reactors_.size())
    return;
  auto h = task.handle;
  task.detach();
  reactors_[reactor_idx]->post([h]() mutable { h.resume(); });
}

} // namespace qbuem
