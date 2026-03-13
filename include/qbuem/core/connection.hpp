#pragma once

/**
 * @file qbuem/core/connection.hpp
 * @brief 단일 TCP 클라이언트 연결을 나타내는 RAII 클래스
 * @defgroup qbuem_connection Connection
 * @ingroup qbuem_core
 *
 * Connection은 fd와 Reactor를 소유하며, 소멸 시 이벤트 해제 및 close(fd) 수행.
 * 연결당 Arena를 내장하여 요청 처리 동안 zero-heap 할당을 지원.
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
