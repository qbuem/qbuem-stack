#pragma once

#include <draco/common.hpp>
#include <draco/core/arena.hpp>
#include <draco/core/reactor.hpp>
#include <memory>
#include <vector>

namespace draco {

/**
 * @brief Represents a single client connection.
 */
class Connection {
public:
  Connection(int fd, Reactor *reactor) : fd_(fd), reactor_(reactor) {}
  ~Connection() {
    if (fd_ != -1) {
      reactor_->unregister_event(fd_, EventType::Read);
      close(fd_);
    }
  }

  int fd() const { return fd_; }
  Arena &arena() { return arena_; }

private:
  int fd_;
  Reactor *reactor_;
  Arena arena_{16 * 1024}; // Per-connection arena
};

} // namespace draco
