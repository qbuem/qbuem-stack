#include <qbuem/core/epoll_reactor.hpp>

#include <cerrno>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <vector>

namespace qbuem {

EpollReactor::EpollReactor() {
  epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd_ == -1) {
    throw std::runtime_error("epoll_create1 failed");
  }

  event_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (event_fd_ == -1) {
    close(epoll_fd_);
    throw std::runtime_error("eventfd failed");
  }

  struct epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = event_fd_;
  if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, event_fd_, &ev) == -1) {
    close(event_fd_);
    close(epoll_fd_);
    throw std::runtime_error("epoll_ctl for eventfd failed");
  }
}

EpollReactor::~EpollReactor() {
  // Drain pending work queue: coroutine frames spawned via dispatcher.spawn()
  // but never executed (because run() was never called) would otherwise leak.
  // Channels should already be closed before the reactor is destroyed, so
  // any blocked recv()/send() awaiters will return immediately on resume.
  // We set ourselves as the current reactor so Reactor::current() is valid
  // for any inline wakeup paths (e.g. Yield awaiter in Action::drain).
  Reactor::set_current(this);
  for (int drain_pass = 0; drain_pass < 64; ++drain_pass) {
    std::vector<std::function<void()>> local;
    {
      std::lock_guard<std::mutex> lock(work_mutex_);
      if (work_queue_.empty()) break;
      local.swap(work_queue_);
    }
    for (auto &fn : local)
      fn();
  }
  Reactor::set_current(nullptr);

  // Close all timerfd handles
  for (auto &[id, entry] : timers_by_id_) {
    close(entry.timerfd);
  }
  if (event_fd_ != -1) {
    close(event_fd_);
  }
  if (epoll_fd_ != -1) {
    close(epoll_fd_);
  }
}

// Helper: add or modify an fd in the epoll instance based on current callbacks.
void EpollReactor::update_epoll(int fd, bool add) {
  uint32_t events = 0;
  auto it = fd_callbacks_.find(fd);
  if (it != fd_callbacks_.end()) {
    if (it->second.read_cb)
      events |= EPOLLIN;
    if (it->second.write_cb)
      events |= EPOLLOUT;
  }

  struct epoll_event ev{};
  ev.data.fd = fd;
  ev.events = events;

  int op = add ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
  if (epoll_ctl(epoll_fd_, op, fd, &ev) == -1 && !add) {
    // Might have been removed already – ignore
  }
}

Result<void> EpollReactor::register_event(int fd, EventType type,
                                          std::function<void(int)> callback) {
  bool is_new = (fd_callbacks_.find(fd) == fd_callbacks_.end());
  auto &cbs = fd_callbacks_[fd];

  if (type == EventType::Read) {
    cbs.read_cb = std::move(callback);
  } else {
    cbs.write_cb = std::move(callback);
  }

  update_epoll(fd, is_new);
  return {};
}

Result<void> EpollReactor::unregister_event(int fd, EventType type) {
  auto it = fd_callbacks_.find(fd);
  if (it == fd_callbacks_.end())
    return {};

  if (type == EventType::Read) {
    it->second.read_cb = nullptr;
  } else {
    it->second.write_cb = nullptr;
  }

  if (!it->second.read_cb && !it->second.write_cb) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    fd_callbacks_.erase(it);
  } else {
    update_epoll(fd, false);
  }
  return {};
}

Result<int> EpollReactor::register_timer(int timeout_ms,
                                         std::function<void(int)> callback) {
  int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (tfd == -1) {
    return unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
  }

  struct itimerspec ts{};
  ts.it_value.tv_sec = timeout_ms / 1000;
  ts.it_value.tv_nsec = (timeout_ms % 1000) * 1000000LL;
  // it_interval is zero-initialised so the timer fires once.

  if (timerfd_settime(tfd, 0, &ts, nullptr) == -1) {
    close(tfd);
    return unexpected(std::make_error_code(std::errc::invalid_argument));
  }

  struct epoll_event ev{};
  ev.events = EPOLLIN | EPOLLET;
  ev.data.fd = tfd;
  if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, tfd, &ev) == -1) {
    close(tfd);
    return unexpected(std::make_error_code(std::errc::io_error));
  }

  int timer_id = next_timer_id_++;
  timers_by_id_[timer_id] = {tfd, std::move(callback)};
  timerfd_to_id_[tfd] = timer_id;
  return timer_id;
}

Result<void> EpollReactor::unregister_timer(int timer_id) {
  auto it = timers_by_id_.find(timer_id);
  if (it == timers_by_id_.end())
    return {};

  int tfd = it->second.timerfd;
  epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, tfd, nullptr);
  close(tfd);
  timerfd_to_id_.erase(tfd);
  timers_by_id_.erase(it);
  return {};
}

Result<int> EpollReactor::poll(int timeout_ms) {
  struct epoll_event events[64];
  int nev = epoll_wait(epoll_fd_, events, 64, timeout_ms);
  if (nev == -1) {
    if (errno == EINTR)
      return 0;
    return unexpected(std::make_error_code(std::errc::io_error));
  }

  for (int i = 0; i < nev; ++i) {
    int fd = events[i].data.fd;
    uint32_t ev = events[i].events;

    // Check if this is the internal wake eventfd
    if (fd == event_fd_) {
      uint64_t val = 0;
      ssize_t n = read(event_fd_, &val, sizeof(val)); // drain counter
      (void)n;
      std::vector<std::function<void()>> local;
      {
        std::lock_guard<std::mutex> lock(work_mutex_);
        local.swap(work_queue_);
      }
      for (auto &fn : local)
        fn();
      continue;
    }

    // Check if this is a timerfd
    auto timer_it = timerfd_to_id_.find(fd);
    if (timer_it != timerfd_to_id_.end()) {
      int timer_id = timer_it->second;
      auto entry_it = timers_by_id_.find(timer_id);
      if (entry_it != timers_by_id_.end()) {
        // Copy callback before unregistering to avoid UAF
        auto cb = entry_it->second.callback;
        unregister_timer(timer_id);
        if (cb)
          cb(timer_id);
      }
      continue;
    }

    // Regular I/O event – copy callbacks to avoid UAF if they self-unregister
    auto cb_it = fd_callbacks_.find(fd);
    if (cb_it == fd_callbacks_.end())
      continue;

    FdCallbacks cbs = cb_it->second;
    if ((ev & EPOLLIN) && cbs.read_cb) {
      cbs.read_cb(fd);
    }
    if ((ev & EPOLLOUT) && cbs.write_cb) {
      cbs.write_cb(fd);
    }
  }

  return nev;
}

void EpollReactor::post(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(work_mutex_);
    work_queue_.push_back(std::move(fn));
  }
  // Wake up epoll_wait by writing to eventfd.
  const uint64_t one = 1;
  ssize_t ret = write(event_fd_, &one, sizeof(one)); // best-effort; EAGAIN means already signalled
  (void)ret;
}

void EpollReactor::stop() { running_ = false; }

bool EpollReactor::is_running() const { return running_; }

} // namespace qbuem
