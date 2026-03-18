#pragma once

/**
 * @file qbuem/io/zero_copy.hpp
 * @brief Zero-copy file-to-socket transfer utilities.
 * @ingroup qbuem_io_buffers
 *
 * Provides coroutine wrappers for `sendfile(2)` and `splice(2)` syscalls.
 * Data is moved within the kernel, enabling high-speed transfer without
 * a userspace copy.
 *
 * ### Platform-specific implementation
 * - Linux:  `sendfile(2)` — `<sys/sendfile.h>`, `splice(2)` — `<fcntl.h>`
 * - macOS:  `sendfile(2)` — different signature (`<sys/types.h>`, `<sys/socket.h>`)
 *
 * ### Design principles
 * - Wrapped as coroutine Tasks to provide an async interface
 * - Suspends via Reactor events until out_fd becomes writable
 * - Errors are returned as `Result<size_t>` without exceptions
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>

#include <cerrno>
#include <coroutine>
#include <sys/types.h>

#if defined(__linux__)
#  include <fcntl.h>
#  include <sys/sendfile.h>
#  include <sys/socket.h>
#  include <linux/errqueue.h>
#  include <linux/in.h>    // IP_RECVERR
#  include <linux/in6.h>   // IPV6_RECVERR
#elif defined(__APPLE__)
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <sys/uio.h>
#endif

namespace qbuem::zero_copy {

// ─── sendfile ────────────────────────────────────────────────────────────────

/**
 * @brief Zero-copy file-to-socket transfer via `sendfile(2)`.
 *
 * Sends file data directly to a socket within the kernel.
 * No userspace buffer copy occurs, making it suitable for serving large files.
 *
 * Suspends the coroutine via a Reactor event until `out_fd` (the socket)
 * becomes writable.
 *
 * @param out_fd  Socket file descriptor to write data to (SOCK_STREAM).
 * @param in_fd   File descriptor to read from (regular file).
 * @param offset  Read start offset within `in_fd` (bytes).
 * @param count   Maximum number of bytes to transfer.
 * @returns Actual number of bytes transferred. error_code on failure.
 *
 * @note On Linux, `sendfile(2)` operates in the file-to-socket direction.
 *       On macOS, `sendfile(2)` has a different signature and is handled separately.
 */
inline Task<Result<size_t>> sendfile(int out_fd, int in_fd,
                                     off_t offset, size_t count) {
  struct SendfileAwaiter {
    int out_fd_;
    int in_fd_;
    off_t offset_;
    size_t count_;
    ssize_t result_ = -1;
    int err_        = 0;

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle) {
      auto *reactor = qbuem::Reactor::current();
      if (!reactor) {
        do_sendfile();
        handle.resume();
        return;
      }
      reactor->register_event(
          out_fd_, qbuem::EventType::Write,
          [handle, this](int f) {
            (void)f;
            do_sendfile();
            qbuem::Reactor::current()->unregister_event(out_fd_,
                                                        qbuem::EventType::Write);
            handle.resume();
          });
    }

    void do_sendfile() noexcept {
#if defined(__linux__)
      off_t off = offset_;
      result_ = ::sendfile(out_fd_, in_fd_, &off,
                           static_cast<size_t>(count_));
      err_ = (result_ < 0) ? errno : 0;
#elif defined(__APPLE__)
      off_t len = static_cast<off_t>(count_);
      int rc = ::sendfile(in_fd_, out_fd_, offset_, &len, nullptr, 0);
      if (rc == 0 || (rc == -1 && errno == EAGAIN)) {
        result_ = static_cast<ssize_t>(len);
        err_    = 0;
      } else {
        result_ = -1;
        err_    = errno;
      }
#else
      // Unsupported platform
      result_ = -1;
      err_    = ENOSYS;
#endif
    }

    void await_resume() const noexcept {}
  };

  SendfileAwaiter aw{out_fd, in_fd, offset, count};
  co_await aw;

  if (aw.result_ < 0) {
    co_return unexpected(
        std::error_code(aw.err_, std::system_category()));
  }
  co_return static_cast<size_t>(aw.result_);
}

// ─── splice ──────────────────────────────────────────────────────────────────

/**
 * @brief Zero-copy fd-to-fd transfer via a pipe using `splice(2)`.
 *
 * Moves data between two file descriptors through a kernel pipe.
 * No userspace buffer copy occurs; socket-to-socket transfers are also supported.
 *
 * Suspends the coroutine via a Reactor event until `in_fd` becomes readable.
 *
 * @param in_fd  File descriptor to read from.
 * @param out_fd File descriptor to write to.
 * @param count  Maximum number of bytes to transfer.
 * @returns Actual number of bytes transferred. error_code on failure.
 *
 * @note Linux only. Returns `ENOSYS` on non-Linux platforms.
 */
inline Task<Result<size_t>> splice(int in_fd, int out_fd, size_t count) {
#if !defined(__linux__)
  (void)in_fd; (void)out_fd; (void)count;
  co_return unexpected(std::make_error_code(std::errc::function_not_supported));
#else
  struct SpliceAwaiter {
    int in_fd_;
    int out_fd_;
    size_t count_;
    ssize_t result_ = -1;
    int err_        = 0;
    int pipe_fds_[2] = {-1, -1};

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle) {
      auto *reactor = qbuem::Reactor::current();
      if (!reactor) {
        do_splice();
        handle.resume();
        return;
      }
      reactor->register_event(
          in_fd_, qbuem::EventType::Read,
          [handle, this](int /*f*/) {
            do_splice();
            qbuem::Reactor::current()->unregister_event(in_fd_,
                                                        qbuem::EventType::Read);
            handle.resume();
          });
    }

    void do_splice() noexcept {
      // Create a temporary pipe
      if (::pipe2(pipe_fds_, O_NONBLOCK | O_CLOEXEC) != 0) {
        result_ = -1;
        err_    = errno;
        return;
      }

      // in_fd → pipe (splice)
      ssize_t n1 = ::splice(in_fd_, nullptr, pipe_fds_[1], nullptr,
                            count_, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
      if (n1 <= 0) {
        result_ = n1;
        err_    = (n1 < 0) ? errno : 0;
        ::close(pipe_fds_[0]);
        ::close(pipe_fds_[1]);
        return;
      }

      // pipe → out_fd (splice)
      ssize_t n2 = ::splice(pipe_fds_[0], nullptr, out_fd_, nullptr,
                            static_cast<size_t>(n1),
                            SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
      ::close(pipe_fds_[0]);
      ::close(pipe_fds_[1]);

      result_ = n2;
      err_    = (n2 < 0) ? errno : 0;
    }

    void await_resume() const noexcept {}
  };

  SpliceAwaiter aw{in_fd, out_fd, count};
  co_await aw;

  if (aw.result_ < 0) {
    co_return unexpected(
        std::error_code(aw.err_, std::system_category()));
  }
  co_return static_cast<size_t>(aw.result_);
#endif
}

// ─── send_zerocopy ───────────────────────────────────────────────────────────

/**
 * @brief Zero-copy socket send using the `MSG_ZEROCOPY` flag (Linux 4.14+).
 *
 * The kernel references the userspace buffer directly, avoiding a data copy
 * into the socket send buffer.
 *
 * The socket must have `SO_ZEROCOPY` set (via `setsockopt`) before calling.
 *
 * After the data is consumed by the network stack, the kernel posts a
 * completion notification to the socket's error queue.  Call
 * `wait_zerocopy()` to drain it.
 *
 * Suspends the coroutine until `sockfd` becomes writable via the Reactor.
 *
 * @param sockfd Socket file descriptor to send data on.
 * @param buf    Pointer to the data buffer to transmit.
 * @param len    Number of bytes to transmit.
 * @returns Number of bytes sent on success, or an error_code on failure.
 *
 * @note Linux 4.14+ only.  Returns `ENOSYS` on non-Linux platforms.
 */
inline Task<Result<size_t>> send_zerocopy(int sockfd,
                                           const void *buf, size_t len) {
#if !defined(__linux__)
  (void)sockfd; (void)buf; (void)len;
  co_return unexpected(std::make_error_code(std::errc::function_not_supported));
#else
  struct SendZerocopyAwaiter {
    int         sockfd_;
    const void *buf_;
    size_t      len_;
    ssize_t     result_ = -1;
    int         err_    = 0;

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle) {
      auto *reactor = qbuem::Reactor::current();
      if (!reactor) {
        do_send();
        handle.resume();
        return;
      }
      reactor->register_event(
          sockfd_, qbuem::EventType::Write,
          [handle, this](int /*f*/) {
            do_send();
            qbuem::Reactor::current()->unregister_event(sockfd_,
                                                        qbuem::EventType::Write);
            handle.resume();
          });
    }

    void do_send() noexcept {
      result_ = ::send(sockfd_, buf_, len_, MSG_ZEROCOPY);
      err_    = (result_ < 0) ? errno : 0;
    }

    void await_resume() const noexcept {}
  };

  SendZerocopyAwaiter aw{sockfd, buf, len};
  co_await aw;

  if (aw.result_ < 0) {
    co_return unexpected(
        std::error_code(aw.err_, std::system_category()));
  }
  co_return static_cast<size_t>(aw.result_);
#endif
}

// ─── wait_zerocopy ───────────────────────────────────────────────────────────

/**
 * @brief Wait for a `MSG_ZEROCOPY` completion notification from the errqueue.
 *
 * After a `send_zerocopy()` call the kernel posts a `sock_extended_err`
 * notification to the socket's error queue once the data has been consumed
 * by the network subsystem.
 *
 * This function uses `recvmsg(2)` with `MSG_ERRQUEUE` to drain the
 * notification and inspects the `sock_extended_err` fields `ee_errno` and
 * `ee_origin` to detect transmission failures.
 *
 * Suspends the coroutine until the Reactor reports the socket as readable
 * (errqueue data is reported as readable by epoll).
 *
 * @param sockfd Socket file descriptor whose errqueue should be drained.
 * @returns `void` on success, or an error_code if the kernel reported a
 *          transmission error or if `recvmsg` itself failed.
 *
 * @note Linux only.  Returns `ENOSYS` on non-Linux platforms.
 */
inline Task<Result<void>> wait_zerocopy(int sockfd) {
#if !defined(__linux__)
  (void)sockfd;
  co_return unexpected(std::make_error_code(std::errc::function_not_supported));
#else
  struct WaitZerocopyAwaiter {
    int  sockfd_;
    int  err_ = 0;

    // Storage for recvmsg ancillary data
    // sock_extended_err + sockaddr fit comfortably in 256 bytes.
    char   cmsg_buf_[256];
    bool   kernel_reported_error_ = false;

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle) {
      auto *reactor = qbuem::Reactor::current();
      if (!reactor) {
        do_recvmsg();
        handle.resume();
        return;
      }
      reactor->register_event(
          sockfd_, qbuem::EventType::Read,
          [handle, this](int /*f*/) {
            do_recvmsg();
            qbuem::Reactor::current()->unregister_event(sockfd_,
                                                        qbuem::EventType::Read);
            handle.resume();
          });
    }

    void do_recvmsg() noexcept {
      struct msghdr msg{};
      msg.msg_control    = cmsg_buf_;
      msg.msg_controllen = sizeof(cmsg_buf_);

      // MSG_ERRQUEUE drains one completion notification from the errqueue.
      int rc = ::recvmsg(sockfd_, &msg, MSG_ERRQUEUE);
      if (rc < 0) {
        err_ = errno;
        return;
      }

      // Walk ancillary messages looking for SOL_IP / SOL_IPV6 extended err.
      for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
           cm != nullptr;
           cm = CMSG_NXTHDR(&msg, cm)) {
        if (cm->cmsg_type == IP_RECVERR ||
            cm->cmsg_type == IPV6_RECVERR) {
          auto *ee = reinterpret_cast<struct sock_extended_err *>(
              CMSG_DATA(cm));
          // ee_origin == SO_EE_ORIGIN_ZEROCOPY on normal completion.
          // Any other origin, or a non-zero ee_errno, signals an error.
          if (ee->ee_origin != SO_EE_ORIGIN_ZEROCOPY) {
            err_                  = (ee->ee_errno != 0) ? static_cast<int>(ee->ee_errno) : EIO;
            kernel_reported_error_ = true;
          } else if (ee->ee_errno != 0) {
            err_                  = static_cast<int>(ee->ee_errno);
            kernel_reported_error_ = true;
          }
          return;
        }
      }
      // No recognised cmsg — treat as success (notification consumed).
    }

    void await_resume() const noexcept {}
  };

  WaitZerocopyAwaiter aw{sockfd};
  co_await aw;

  if (aw.err_ != 0) {
    co_return unexpected(
        std::error_code(aw.err_, std::system_category()));
  }
  co_return {};
#endif
}

} // namespace qbuem::zero_copy

/** @} */ // end of qbuem_io_buffers
