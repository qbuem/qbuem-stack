#pragma once

/**
 * @file qbuem/io/async_file.hpp
 * @brief 비동기 파일 I/O — pread/pwrite 기반 코루틴 래퍼.
 * @ingroup qbuem_io_buffers
 *
 * `AsyncFile`은 파일 디스크립터를 RAII로 관리하며
 * `read_at()` / `write_at()` 을 코루틴 Task로 제공합니다.
 *
 * ### 현재 구현
 * - `open()`: 일반 파일 open(2), O_NONBLOCK 설정
 * - `read_at()`: `pread(2)` — 오프셋 기반 읽기, 파일 위치 포인터 불변
 * - `write_at()`: `pwrite(2)` — 오프셋 기반 쓰기, 파일 위치 포인터 불변
 *
 * ### 미래 확장
 * - io_uring IORING_OP_READ / IORING_OP_WRITE로 교체 가능
 * - `io_uring_reactor`가 활성화된 환경에서는 zero-syscall-overhead 경로 사용
 *
 * @note 현재 pread/pwrite는 블로킹 syscall입니다.
 *       고성능 환경에서는 io_uring 구현체 또는 스레드 풀 오프로드를 권장합니다.
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>

#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace qbuem {

/**
 * @brief 비동기 파일 I/O 래퍼.
 *
 * 파일 디스크립터를 RAII로 소유하며 `pread`/`pwrite` 기반의
 * 코루틴 인터페이스를 제공합니다.
 * Move-only 타입이며 소멸 시 파일이 자동으로 닫힙니다.
 *
 * ### 사용 예시
 * @code
 * auto file = co_await AsyncFile::open("data.bin", O_RDONLY);
 * if (!file) { // 에러 처리 }
 *
 * std::array<std::byte, 512> buf{};
 * auto n = co_await file->read_at(buf, 0);
 * if (!n) { // 에러 처리 }
 * @endcode
 */
class AsyncFile {
public:
  /** @brief 유효하지 않은 fd로 초기화. */
  AsyncFile() noexcept : fd_(-1) {}

  /**
   * @brief 기존 fd로 AsyncFile을 구성합니다.
   * @param fd 열려 있는 파일 디스크립터.
   */
  explicit AsyncFile(int fd) noexcept : fd_(fd) {}

  /** @brief 소멸자: 열려 있는 파일을 자동으로 닫습니다. */
  ~AsyncFile() {
    if (fd_ >= 0)
      ::close(fd_);
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
   * @brief 파일을 비동기로 엽니다.
   *
   * `open(2)` syscall로 파일을 열고 성공 시 AsyncFile을 반환합니다.
   * `O_CLOEXEC`는 항상 적용됩니다.
   *
   * @param path  열 파일 경로.
   * @param flags `open(2)` 플래그 (예: `O_RDONLY`, `O_WRONLY|O_CREAT`).
   * @param mode  파일 생성 시 권한 비트 (기본값: 0644).
   * @returns 성공 시 AsyncFile, 실패 시 에러 코드.
   */
  static Task<Result<AsyncFile>> open(std::string_view path,
                                      int flags,
                                      mode_t mode = 0644) {
    // string_view는 null-terminated 보장이 없으므로 임시 복사
    // 경로가 PATH_MAX를 초과하면 에러 반환
    if (path.size() >= 4096) {
      co_return unexpected(
          std::make_error_code(std::errc::filename_too_long));
    }

    char path_buf[4096];
    path.copy(path_buf, path.size());
    path_buf[path.size()] = '\0';

    int fd = ::open(path_buf, flags | O_CLOEXEC, mode);
    if (fd < 0) {
      co_return unexpected(
          std::error_code(errno, std::system_category()));
    }
    co_return AsyncFile(fd);
  }

  // ─── 비동기 I/O ─────────────────────────────────────────────────────────

  /**
   * @brief 지정된 오프셋에서 데이터를 읽습니다 (pread).
   *
   * 파일의 현재 위치 포인터를 변경하지 않습니다.
   * 여러 코루틴이 동시에 서로 다른 오프셋을 읽어도 안전합니다.
   *
   * @param buf    읽은 데이터를 저장할 버퍼.
   * @param offset 파일 내 읽기 시작 오프셋 (바이트).
   * @returns 읽은 바이트 수. EOF면 0. 에러 시 error_code.
   */
  Task<Result<size_t>> read_at(MutableBufferView buf, off_t offset) {
    ssize_t n = ::pread(fd_,
                        reinterpret_cast<void *>(buf.data()),
                        buf.size(),
                        offset);
    if (n < 0) {
      co_return unexpected(
          std::error_code(errno, std::system_category()));
    }
    co_return static_cast<size_t>(n);
  }

  /**
   * @brief 지정된 오프셋에 데이터를 씁니다 (pwrite).
   *
   * 파일의 현재 위치 포인터를 변경하지 않습니다.
   *
   * @param buf    쓸 데이터 버퍼.
   * @param offset 파일 내 쓰기 시작 오프셋 (바이트).
   * @returns 쓴 바이트 수. 에러 시 error_code.
   */
  Task<Result<size_t>> write_at(BufferView buf, off_t offset) {
    ssize_t n = ::pwrite(fd_,
                         reinterpret_cast<const void *>(buf.data()),
                         buf.size(),
                         offset);
    if (n < 0) {
      co_return unexpected(
          std::error_code(errno, std::system_category()));
    }
    co_return static_cast<size_t>(n);
  }

  /**
   * @brief 파일을 닫습니다.
   *
   * 이미 닫혀 있는 경우 no-op입니다.
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
      co_return unexpected(
          std::error_code(errno, std::system_category()));
    }
    co_return Result<void>::ok();
  }

  // ─── 접근자 ─────────────────────────────────────────────────────────────

  /**
   * @brief 내부 파일 디스크립터를 반환합니다.
   * @returns 파일 fd. 유효하지 않으면 -1.
   */
  [[nodiscard]] int fd() const noexcept { return fd_; }

  /**
   * @brief 파일이 열려 있는지 확인합니다.
   * @returns fd >= 0이면 true.
   */
  [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }

private:
  /** @brief 관리 중인 파일 디스크립터. */
  int fd_;
};

} // namespace qbuem

/** @} */ // end of qbuem_io_buffers
