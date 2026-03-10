#include <draco/core/kqueue_reactor.hpp>

#include <cerrno>
#include <stdexcept>
#include <sys/event.h>
#include <unistd.h>

namespace draco {

KqueueReactor::KqueueReactor() {
  kq_fd_ = kqueue();
  if (kq_fd_ == -1) {
    throw std::runtime_error("failed to create kqueue");
  }
}

KqueueReactor::~KqueueReactor() {
  stop();
  if (kq_fd_ != -1) {
    close(kq_fd_);
  }
}

Result<void> KqueueReactor::register_event(int fd, EventType type,
                                           std::function<void(int)> callback) {
  struct kevent ev;
  int16_t filter = (type == EventType::Read) ? EVFILT_READ : EVFILT_WRITE;
  EV_SET(&ev, fd, filter, EV_ADD | EV_ENABLE, 0, 0, nullptr);

  if (kevent(kq_fd_, &ev, 1, nullptr, 0, nullptr) == -1) {
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  }

  if (type == EventType::Read) {
    callbacks_[fd].read_cb = std::move(callback);
  } else {
    callbacks_[fd].write_cb = std::move(callback);
  }
  return {};
}

Result<void> KqueueReactor::unregister_event(int fd, EventType type) {
  struct kevent ev;
  int16_t filter = (type == EventType::Read) ? EVFILT_READ : EVFILT_WRITE;
  EV_SET(&ev, fd, filter, EV_DELETE, 0, 0, nullptr);
  kevent(kq_fd_, &ev, 1, nullptr, 0, nullptr);

  auto it = callbacks_.find(fd);
  if (it != callbacks_.end()) {
    if (type == EventType::Read) {
      it->second.read_cb = nullptr;
    } else {
      it->second.write_cb = nullptr;
    }

    if (!it->second.read_cb && !it->second.write_cb) {
      callbacks_.erase(it);
    }
  }
  return {};
}

Result<int> KqueueReactor::register_timer(int timeout_ms,
                                          std::function<void(int)> callback) {
  int timer_id = next_timer_id_++;
  struct kevent ev;
  EV_SET(&ev, timer_id, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT, 0,
         timeout_ms, nullptr);

  if (kevent(kq_fd_, &ev, 1, nullptr, 0, nullptr) == -1) {
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  }

  callbacks_[timer_id].timer_cb = std::move(callback);
  return timer_id;
}

Result<void> KqueueReactor::unregister_timer(int timer_id) {
  struct kevent ev;
  EV_SET(&ev, timer_id, EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
  kevent(kq_fd_, &ev, 1, nullptr, 0, nullptr);
  callbacks_.erase(timer_id);
  return {};
}

Result<int> KqueueReactor::poll(int timeout_ms) {
  struct kevent events[64];
  struct timespec ts{};
  struct timespec *timeout_ptr = nullptr;

  if (timeout_ms >= 0) {
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (timeout_ms % 1000) * 1000000;
    timeout_ptr = &ts;
  }

  int nev = kevent(kq_fd_, nullptr, 0, events, 64, timeout_ptr);
  if (nev == -1) {
    if (errno == EINTR)
      return 0;
    return std::unexpected(std::make_error_code(std::errc::io_error));
  }

  for (int i = 0; i < nev; ++i) {
    int ident = static_cast<int>(events[i].ident);
    int16_t filter = events[i].filter;

    auto callback_it = callbacks_.find(ident);
    if (callback_it != callbacks_.end()) {
      // Copy callbacks to avoid UAF if a callback unregisters itself.
      auto cbs = callback_it->second;
      if (filter == EVFILT_READ && cbs.read_cb) {
        cbs.read_cb(ident);
      } else if (filter == EVFILT_WRITE && cbs.write_cb) {
        cbs.write_cb(ident);
      } else if (filter == EVFILT_TIMER && cbs.timer_cb) {
        // EV_ONESHOT: the timer fires once and is already removed from kqueue.
        callbacks_.erase(ident);
        cbs.timer_cb(ident);
      }
    }
  }

  return nev;
}

void KqueueReactor::stop() { running_ = false; }

bool KqueueReactor::is_running() const { return running_; }

} // namespace draco
