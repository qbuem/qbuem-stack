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

} // namespace qbuem::zero_copy

/** @} */ // end of qbuem_io_buffers
