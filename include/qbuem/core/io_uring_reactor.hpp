#pragma once

#include <qbuem/core/reactor.hpp>

#include <functional>
#include <unordered_map>

// Forward-declare the liburing type so consumers don't need liburing.h
struct io_uring;

namespace qbuem {

/**
 * @brief Linux io_uring Reactor.
 *
 * Uses io_uring's POLL_ADD operation for I/O readiness notification and
 * IORING_OP_TIMEOUT for timer support. Both are naturally one-shot; the
 * reactor re-submits them automatically to maintain persistent-event semantics.
 *
 * The complete io_uring ring is owned internally and the full liburing API is
 * only needed in the .cpp translation unit.
 */
class IOUringReactor final : public Reactor {
public:
  static constexpr unsigned QUEUE_DEPTH = 256;

  IOUringReactor();
  ~IOUringReactor() override;

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
  // Pimpl: avoids exposing <liburing.h> in the public header.
  struct Impl;
  Impl *impl_;
};

} // namespace qbuem
