#pragma once

#include <qbuem/core/reactor.hpp>

#include <functional>
#include <span>
#include <sys/uio.h>
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

  /**
   * @brief Returns whether SQPOLL mode is active.
   *
   * true if the kernel accepted IORING_SETUP_SQPOLL.
   * false if fallen back to normal mode (e.g., insufficient permissions).
   */
  bool is_sqpoll() const noexcept;

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

  void post(std::function<void()> fn) override;

  Result<void> register_signal(int sig,
                               std::function<void(int)> callback) override;
  Result<void> unregister_signal(int sig) override;

  // -------------------------------------------------------------------------
  // Fixed Buffer API  (io_uring_register_buffers — direct DMA writes)
  // -------------------------------------------------------------------------

  /**
   * @brief Register an `iovec` array as fixed buffers with the kernel.
   *
   * Registered buffers are pinned in the page table, allowing DMA operations
   * to use them directly without copying. Reference them by buffer index in
   * `read_fixed()` / `write_fixed()` calls.
   *
   * @param iovecs iovec array to register. Must remain valid until
   *               `unregister_fixed_buffers()` is called.
   * @returns `ok()` or an error code.
   *
   * @note If buffers are already registered, call `unregister_fixed_buffers()` first.
   */
  Result<void> register_fixed_buffers(std::span<const iovec> iovecs);

  /**
   * @brief Unregister all currently registered fixed buffers.
   */
  void unregister_fixed_buffers() noexcept;

  /**
   * @brief Return the number of currently registered fixed buffers.
   */
  size_t fixed_buffer_count() const noexcept;

  /**
   * @brief `IORING_OP_READ_FIXED` — asynchronous read into a fixed buffer.
   *
   * Unlike a regular read(), the kernel already knows the buffer, so no copy occurs.
   *
   * @param fd          File descriptor to read from.
   * @param buf_idx     Buffer index specified at `register_fixed_buffers()` time.
   * @param buf         Slice within the buffer (address + length must be within
   *                    the registered range).
   * @param file_offset -1 for streaming fds; actual offset for file fds.
   * @param callback    Called on completion. Positive = bytes read, negative = errno.
   */
  Result<void> read_fixed(int fd, int buf_idx, std::span<std::byte> buf,
                          int64_t file_offset,
                          std::function<void(int)> callback);

  /**
   * @brief `IORING_OP_WRITE_FIXED` — asynchronous write from a fixed buffer.
   *
   * @param fd          File descriptor to write to.
   * @param buf_idx     Registered buffer index.
   * @param buf         Buffer slice.
   * @param file_offset -1 for streaming fds.
   * @param callback    Called on completion. Positive = bytes written, negative = errno.
   */
  Result<void> write_fixed(int fd, int buf_idx, std::span<const std::byte> buf,
                           int64_t file_offset,
                           std::function<void(int)> callback);

  // -------------------------------------------------------------------------
  // Buffer Ring API  (IORING_OP_PROVIDE_BUFFERS — kernel automatic buffer selection)
  // -------------------------------------------------------------------------

  /**
   * @brief Register a Buffer Ring that lets the kernel automatically select
   *        recv buffers.
   *
   * Passes a buffer pool to the kernel via `IORING_OP_PROVIDE_BUFFERS`.
   * On `recv_buffered()` calls the kernel picks an available buffer and
   * includes the chosen buffer ID in the CQE flags.
   *
   * @param bgid         Buffer group ID (0~65535). Must be unique within the ring.
   * @param buf_size     Size of each individual buffer (bytes).
   * @param buf_count    Number of buffers. Powers of two are recommended.
   * @returns `ok()` or an error code.
   *
   * @note Requires Linux 5.19+ / liburing 2.2+.
   */
  Result<void> register_buf_ring(uint16_t bgid, size_t buf_size,
                                 size_t buf_count);

  /**
   * @brief Unregister a Buffer Ring.
   *
   * @param bgid Group ID specified at `register_buf_ring()` time.
   */
  void unregister_buf_ring(uint16_t bgid) noexcept;

  /**
   * @brief Asynchronous recv using a Buffer Ring.
   *
   * The kernel automatically selects a free buffer from the `bgid` group and
   * fills it with received data. On completion, invokes
   * `callback(bytes_received, buf_id, buf_ptr)`. After consuming the buffer
   * inside the callback, `return_buf_to_ring(bgid, buf_id)` must be called.
   *
   * @param fd       Socket to receive from.
   * @param bgid     Buffer group ID.
   * @param callback Completion callback. bytes < 0 indicates error; buf_ptr == nullptr.
   */
  Result<void> recv_buffered(int fd, uint16_t bgid,
                             std::function<void(int, uint16_t, void *)> callback);

  /**
   * @brief Return a consumed buffer back to the Buffer Ring.
   *
   * Must be called after processing the buffer inside a `recv_buffered()` callback.
   * Failure to return a buffer exhausts the pool and prevents reuse.
   *
   * @param bgid   Buffer group ID.
   * @param buf_id Buffer ID to return (passed to the recv callback).
   */
  void return_buf_to_ring(uint16_t bgid, uint16_t buf_id) noexcept;

private:
  // Pimpl: avoids exposing <liburing.h> in the public header.
  struct Impl;
  Impl *impl_;
};

} // namespace qbuem
