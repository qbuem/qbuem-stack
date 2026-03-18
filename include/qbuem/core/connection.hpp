#pragma once

/**
 * @file qbuem/core/connection.hpp
 * @brief RAII class representing a single TCP client connection
 * @defgroup qbuem_connection Connection
 * @ingroup qbuem_core
 *
 * Connection owns a fd and a Reactor, and on destruction unregisters events
 * and calls close(fd). Embeds a per-connection Arena to support zero-heap
 * allocation during request processing.
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/arena.hpp>
#include <qbuem/core/reactor.hpp>
#include <memory>
#include <vector>

namespace qbuem {

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

} // namespace qbuem

/** @} */
