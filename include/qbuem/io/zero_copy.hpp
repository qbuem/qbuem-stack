#pragma once

/**
 * @file qbuem/io/zero_copy.hpp
 * @brief 제로 카피 파일-소켓 전송 유틸리티.
 * @ingroup qbuem_io_buffers
 *
 * `sendfile(2)` 및 `splice(2)` syscall의 코루틴 래퍼를 제공합니다.
 * 커널 내에서 데이터를 이동하여 유저스페이스 복사 없이 고속 전송이 가능합니다.
 *
 * ### 플랫폼별 구현
 * - Linux:  `sendfile(2)` — `<sys/sendfile.h>`, `splice(2)` — `<fcntl.h>`
 * - macOS:  `sendfile(2)` — 다른 시그니처 (`<sys/types.h>`, `<sys/socket.h>`)
 *
 * ### 설계 원칙
 * - 코루틴 Task로 래핑하여 비동기 인터페이스 제공
 * - Reactor 이벤트를 활용하여 out_fd가 쓰기 가능해질 때까지 대기
 * - 에러는 예외 없이 `Result<size_t>`로 반환
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
#elif defined(__APPLE__)
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <sys/uio.h>
#endif

namespace qbuem::zero_copy {

// ─── sendfile ────────────────────────────────────────────────────────────────

/**
 * @brief `sendfile(2)` 기반 파일→소켓 제로 카피 전송.
 *
 * 커널 내에서 파일 데이터를 소켓으로 직접 전송합니다.
 * 유저스페이스 버퍼 복사가 없어 대용량 파일 서빙에 적합합니다.
 *
 * `out_fd`(소켓)가 쓰기 가능해질 때까지 Reactor 이벤트를 대기합니다.
 *
 * @param out_fd  데이터를 쓸 소켓 파일 디스크립터 (SOCK_STREAM).
 * @param in_fd   읽을 파일 디스크립터 (일반 파일).
 * @param offset  `in_fd`의 읽기 시작 오프셋 (바이트).
 * @param count   전송할 최대 바이트 수.
 * @returns 실제 전송한 바이트 수. 에러 시 error_code.
 *
 * @note Linux에서만 `sendfile(2)`이 파일→소켓 방향으로 동작합니다.
 *       macOS에서는 `sendfile(2)` 시그니처가 다르므로 별도 처리됩니다.
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
      // 지원하지 않는 플랫폼
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
 * @brief `splice(2)` 기반 파이프를 통한 fd→fd 제로 카피 전송.
 *
 * 두 파일 디스크립터 사이에 커널 파이프를 통해 데이터를 이동합니다.
 * 유저스페이스 버퍼 복사가 없으며 소켓→소켓 전송도 지원합니다.
 *
 * `in_fd`가 읽기 가능해질 때까지 Reactor 이벤트를 대기합니다.
 *
 * @param in_fd  읽을 파일 디스크립터.
 * @param out_fd 쓸 파일 디스크립터.
 * @param count  전송할 최대 바이트 수.
 * @returns 실제 전송한 바이트 수. 에러 시 error_code.
 *
 * @note Linux 전용입니다. 비Linux 플랫폼에서는 `ENOSYS`를 반환합니다.
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
      // 임시 파이프 생성
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
 * @brief `MSG_ZEROCOPY` 플래그를 이용한 제로 카피 소켓 송신.
 *        Zero-copy socket send using the `MSG_ZEROCOPY` flag (Linux 4.14+).
 *
 * 커널이 유저스페이스 버퍼를 직접 참조하여 복사 없이 네트워크로 전송합니다.
 * The kernel references the userspace buffer directly, avoiding a data copy
 * into the socket send buffer.
 *
 * 소켓에 `SO_ZEROCOPY` 옵션이 설정되어 있어야 합니다.
 * The socket must have `SO_ZEROCOPY` set (via `setsockopt`) before calling.
 *
 * 전송이 완료되면 커널이 errqueue로 완료 알림을 보냅니다.
 * After the data is consumed by the network stack, the kernel posts a
 * completion notification to the socket's error queue.  Call
 * `wait_zerocopy()` to drain it.
 *
 * `sockfd`가 쓰기 가능해질 때까지 Reactor 이벤트를 대기합니다.
 * Suspends the coroutine until `sockfd` becomes writable via the Reactor.
 *
 * @param sockfd 데이터를 전송할 소켓 파일 디스크립터.
 *               Socket file descriptor to send data on.
 * @param buf    전송할 데이터 버퍼의 시작 주소.
 *               Pointer to the data buffer to transmit.
 * @param len    전송할 바이트 수.
 *               Number of bytes to transmit.
 * @returns 실제 전송한 바이트 수. 에러 시 error_code.
 *          Number of bytes sent on success, or an error_code on failure.
 *
 * @note Linux 4.14 이상 전용입니다. 비Linux 플랫폼에서는 `ENOSYS`를 반환합니다.
 *       Linux 4.14+ only.  Returns `ENOSYS` on non-Linux platforms.
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
 * @brief `MSG_ZEROCOPY` 완료 알림을 errqueue에서 대기 및 소비.
 *        Wait for a `MSG_ZEROCOPY` completion notification from the errqueue.
 *
 * `send_zerocopy()` 호출 후 커널이 errqueue에 완료 알림을 게시합니다.
 * After a `send_zerocopy()` call the kernel posts a `sock_extended_err`
 * notification to the socket's error queue once the data has been consumed
 * by the network subsystem.
 *
 * 이 함수는 `MSG_ERRQUEUE` 플래그와 함께 `recvmsg(2)`를 사용하여
 * 알림을 수신하고, `sock_extended_err`의 `ee_errno`/`ee_origin` 필드를
 * 검사하여 실패 여부를 판별합니다.
 * This function uses `recvmsg(2)` with `MSG_ERRQUEUE` to drain the
 * notification and inspects the `sock_extended_err` fields `ee_errno` and
 * `ee_origin` to detect transmission failures.
 *
 * `sockfd`에 읽을 수 있는 errqueue 항목이 생길 때까지 Reactor 이벤트를 대기합니다.
 * Suspends the coroutine until the Reactor reports the socket as readable
 * (errqueue data is reported as readable by epoll).
 *
 * @param sockfd 완료 알림을 기다릴 소켓 파일 디스크립터.
 *               Socket file descriptor whose errqueue should be drained.
 * @returns 성공 시 void. 커널이 전송 실패를 보고한 경우 error_code.
 *          `void` on success, or an error_code if the kernel reported a
 *          transmission error or if `recvmsg` itself failed.
 *
 * @note Linux 전용입니다. 비Linux 플랫폼에서는 `ENOSYS`를 반환합니다.
 *       Linux only.  Returns `ENOSYS` on non-Linux platforms.
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
