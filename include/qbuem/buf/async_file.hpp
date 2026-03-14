#pragma once

/**
 * @file qbuem/buf/async_file.hpp
 * @brief 비동기 파일 I/O 타입.
 * @ingroup qbuem_buf
 *
 * `AsyncFile`은 파일 디스크립터를 RAII로 관리하며,
 * `pread(2)`/`pwrite(2)` 기반의 오프셋 I/O 코루틴 인터페이스를 제공합니다.
 *
 * ### 설계 원칙
 * - Move-only: 복사 불가, 소멸자에서 fd 자동 close
 * - `read_at` / `write_at`: 오프셋 기반 — 파일 위치 포인터 불변
 * - Reactor Read/Write 이벤트를 통한 비동기 대기
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>

#include <cerrno>
#include <coroutine>
#include <fcntl.h>
#include <span>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace qbuem {

/**
 * @brief 비동기 파일 I/O 래퍼.
 *
 * 파일 디스크립터를 래핑하여 오프셋 기반의 비동기 읽기/쓰기 인터페이스를 제공합니다.
 * Reactor 이벤트를 통해 파일이 준비될 때까지 코루틴을 중단합니다.
 *
 * @note 일반 파일은 항상 즉시 읽기/쓰기 가능하므로 Reactor 등록 없이 직접 syscall을
 *       수행합니다. Reactor 대기는 파이프, 특수 파일 등에서 유효합니다.
 *
 * ### 사용 예시
 * @code
 * auto f = AsyncFile::open("/tmp/data.bin", O_RDONLY);
 * if (!f) { // 에러 처리 }
 *
 * std::array<std::byte, 4096> buf{};
 * auto n = co_await f->read_at(buf, 0);
 * @endcode
 */
class AsyncFile {
public:
  /** @brief 유효하지 않은 fd로 초기화. */
  AsyncFile() noexcept : fd_(-1) {}

  /**
   * @brief 기존 fd로 AsyncFile을 구성합니다.
   * @param fd 파일 디스크립터.
   */
  explicit AsyncFile(int fd) noexcept : fd_(fd) {}

  /** @brief 소멸자: 열린 파일을 자동으로 닫습니다. */
  ~AsyncFile() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  /** @brief 복사 생성자 삭제 (Move-only). */
  AsyncFile(const AsyncFile &) = delete;

  /** @brief 복사 대입 삭제 (Move-only). */
  AsyncFile &operator=(const AsyncFile &) = delete;

  /** @brief 이동 생성자. */
  AsyncFile(AsyncFile &&other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
  }

  /** @brief 이동 대입 연산자. */
  AsyncFile &operator=(AsyncFile &&other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) ::close(fd_);
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  // ─── 팩토리 메서드 ──────────────────────────────────────────────────────

  /**
   * @brief 파일을 열어 AsyncFile을 생성합니다.
   *
   * @param path  파일 경로.
   * @param flags `open(2)` 플래그 (O_RDONLY, O_WRONLY, O_RDWR, O_CREAT 등).
   * @param mode  파일 생성 시 권한 비트 (O_CREAT 플래그와 함께 사용, 기본 0).
   * @returns 성공 시 AsyncFile, 실패 시 에러 코드.
   */
  static Result<AsyncFile> open(const char *path, int flags,
                                mode_t mode = 0) noexcept {
    int fd = ::open(path, flags | O_CLOEXEC, mode);
    if (fd < 0)
      return unexpected(std::error_code(errno, std::system_category()));
    return AsyncFile(fd);
  }

  // ─── 비동기 I/O ─────────────────────────────────────────────────────────

  /**
   * @brief 파일의 지정된 오프셋에서 데이터를 비동기로 읽습니다.
   *
   * `pread(2)` 기반이므로 파일 오프셋 포인터를 변경하지 않습니다.
   * 파일 fd가 블로킹 상태일 경우 Reactor 읽기 이벤트를 등록하여 대기합니다.
   * 일반 파일은 대기 없이 즉시 실행됩니다.
   *
   * @param buf    수신 데이터를 저장할 버퍼.
   * @param offset 읽기 시작 파일 오프셋 (바이트).
   * @returns 읽은 바이트 수. EOF면 0. 에러 시 error_code.
   */
  Task<Result<size_t>> read_at(std::span<std::byte> buf, off_t offset) {
    struct PreadAwaiter {
      int fd_;
      void *buf_;
      size_t count_;
      off_t offset_;
      ssize_t result_ = -1;
      int err_ = 0;

      bool await_ready() const noexcept { return false; }

      void await_suspend(std::coroutine_handle<> handle) {
        auto *reactor = Reactor::current();
        if (!reactor) {
          result_ = ::pread(fd_, buf_, count_, offset_);
          err_ = (result_ < 0) ? errno : 0;
          handle.resume();
          return;
        }
        // 일단 논블로킹 시도
        ssize_t n = ::pread(fd_, buf_, count_, offset_);
        if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
          result_ = n;
          err_ = (n < 0) ? errno : 0;
          handle.resume();
          return;
        }
        // EAGAIN: Reactor 읽기 이벤트 대기
        reactor->register_event(fd_, EventType::Read, [handle, this](int f) {
          result_ = ::pread(f, buf_, count_, offset_);
          err_ = (result_ < 0) ? errno : 0;
          Reactor::current()->unregister_event(f, EventType::Read);
          handle.resume();
        });
      }

      void await_resume() const noexcept {}
    };

    PreadAwaiter aw{fd_, buf.data(), buf.size(), offset};
    co_await aw;

    if (aw.result_ < 0) {
      co_return unexpected(std::error_code(aw.err_, std::system_category()));
    }
    co_return static_cast<size_t>(aw.result_);
  }

  /**
   * @brief 파일의 지정된 오프셋에 데이터를 비동기로 씁니다.
   *
   * `pwrite(2)` 기반이므로 파일 오프셋 포인터를 변경하지 않습니다.
   * 파일 fd가 쓰기 블로킹 상태일 경우 Reactor 쓰기 이벤트를 등록하여 대기합니다.
   *
   * @param buf    전송할 데이터 버퍼.
   * @param offset 쓰기 시작 파일 오프셋 (바이트).
   * @returns 전송한 바이트 수. 에러 시 error_code.
   */
  Task<Result<size_t>> write_at(std::span<const std::byte> buf, off_t offset) {
    struct PwriteAwaiter {
      int fd_;
      const void *buf_;
      size_t count_;
      off_t offset_;
      ssize_t result_ = -1;
      int err_ = 0;

      bool await_ready() const noexcept { return false; }

      void await_suspend(std::coroutine_handle<> handle) {
        auto *reactor = Reactor::current();
        if (!reactor) {
          result_ = ::pwrite(fd_, buf_, count_, offset_);
          err_ = (result_ < 0) ? errno : 0;
          handle.resume();
          return;
        }
        // 일단 논블로킹 시도
        ssize_t n = ::pwrite(fd_, buf_, count_, offset_);
        if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
          result_ = n;
          err_ = (n < 0) ? errno : 0;
          handle.resume();
          return;
        }
        // EAGAIN: Reactor 쓰기 이벤트 대기
        reactor->register_event(fd_, EventType::Write, [handle, this](int f) {
          result_ = ::pwrite(f, buf_, count_, offset_);
          err_ = (result_ < 0) ? errno : 0;
          Reactor::current()->unregister_event(f, EventType::Write);
          handle.resume();
        });
      }

      void await_resume() const noexcept {}
    };

    PwriteAwaiter aw{fd_, buf.data(), buf.size(), offset};
    co_await aw;

    if (aw.result_ < 0) {
      co_return unexpected(std::error_code(aw.err_, std::system_category()));
    }
    co_return static_cast<size_t>(aw.result_);
  }

  /**
   * @brief 파일을 비동기로 닫습니다.
   *
   * `close(2)` 호출 후 내부 fd를 -1로 설정합니다.
   * 이미 닫힌 상태에서 호출하면 no-op입니다.
   *
   * @returns 성공 시 `Result<void>::ok()`, 실패 시 에러 코드.
   */
  Task<Result<void>> close() {
    if (fd_ < 0) {
      co_return Result<void>::ok();
    }
    int fd = fd_;
    fd_ = -1;
    if (::close(fd) != 0) {
      co_return unexpected(std::error_code(errno, std::system_category()));
    }
    co_return Result<void>::ok();
  }

  // ─── 접근자 ─────────────────────────────────────────────────────────────

  /**
   * @brief 내부 파일 디스크립터를 반환합니다.
   * @returns 파일 fd. 유효하지 않으면 -1.
   */
  int fd() const noexcept { return fd_; }

private:
  /** @brief 관리 중인 파일 디스크립터. */
  int fd_;
};

} // namespace qbuem

/** @} */ // end of qbuem_buf
