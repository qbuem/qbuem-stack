#pragma once

/**
 * @file qbuem/io/async_file.hpp
 * @brief Asynchronous file I/O — coroutine wrapper based on pread/pwrite.
 * @ingroup qbuem_io_buffers
 *
 * `AsyncFile` manages a file descriptor with RAII and exposes
 * `read_at()` / `write_at()` as coroutine Tasks.
 *
 * ### Current implementation
 * - `open()`: standard file open(2), sets O_NONBLOCK
 * - `read_at()`: `pread(2)` — offset-based read, file position pointer unchanged
 * - `write_at()`: `pwrite(2)` — offset-based write, file position pointer unchanged
 *
 * ### Future extensions
 * - Replaceable with io_uring IORING_OP_READ / IORING_OP_WRITE
 * - When `io_uring_reactor` is active, uses the zero-syscall-overhead path
 *
 * @note Currently pread/pwrite are blocking syscalls.
 *       For high-performance environments, an io_uring implementation or
 *       thread-pool offload is recommended.
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
 * @brief Asynchronous file I/O wrapper.
 *
 * Owns a file descriptor with RAII and provides a coroutine interface
 * based on `pread`/`pwrite`.
 * Move-only type; the file is closed automatically upon destruction.
 *
 * ### Usage example
 * @code
 * auto file = co_await AsyncFile::open("data.bin", O_RDONLY);
 * if (!file) { // error handling }
 *
 * std::array<std::byte, 512> buf{};
 * auto n = co_await file->read_at(buf, 0);
 * if (!n) { // error handling }
 * @endcode
 */
class AsyncFile {
public:
  /** @brief Initializes with an invalid fd. */
  AsyncFile() noexcept : fd_(-1) {}

  /**
   * @brief Constructs an AsyncFile from an existing fd.
   * @param fd An open file descriptor.
   */
  explicit AsyncFile(int fd) noexcept : fd_(fd) {}

  /** @brief Destructor: automatically closes the open file. */
  ~AsyncFile() {
    if (fd_ >= 0)
      ::close(fd_);
  }

  /** @brief Copy constructor deleted (Move-only). */
  AsyncFile(const AsyncFile &) = delete;

  /** @brief Copy assignment deleted (Move-only). */
  AsyncFile &operator=(const AsyncFile &) = delete;

  /** @brief Move constructor. */
  AsyncFile(AsyncFile &&other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
  }

  /** @brief Move assignment operator. */
  AsyncFile &operator=(AsyncFile &&other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) ::close(fd_);
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  // ─── Factory methods ────────────────────────────────────────────────────

  /**
   * @brief Opens a file asynchronously.
   *
   * Opens the file using the `open(2)` syscall and returns an AsyncFile on success.
   * `O_CLOEXEC` is always applied.
   *
   * @param path  Path of the file to open.
   * @param flags `open(2)` flags (e.g. `O_RDONLY`, `O_WRONLY|O_CREAT`).
   * @param mode  Permission bits when creating a file (default: 0644).
   * @returns AsyncFile on success, or an error code on failure.
   */
  static Task<Result<AsyncFile>> open(std::string_view path,
                                      int flags,
                                      mode_t mode = 0644) {
    // string_view does not guarantee null-termination, so copy into a temporary buffer.
    // Return an error if the path exceeds PATH_MAX.
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

  // ─── Async I/O ──────────────────────────────────────────────────────────

  /**
   * @brief Reads data from the specified offset (pread).
   *
   * Does not change the file's current position pointer.
   * Safe for multiple coroutines to read different offsets concurrently.
   *
   * @param buf    Buffer to store the read data.
   * @param offset Read start offset within the file (bytes).
   * @returns Number of bytes read. 0 on EOF. error_code on failure.
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
   * @brief Writes data at the specified offset (pwrite).
   *
   * Does not change the file's current position pointer.
   *
   * @param buf    Data buffer to write.
   * @param offset Write start offset within the file (bytes).
   * @returns Number of bytes written. error_code on failure.
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
   * @brief Closes the file.
   *
   * No-op if the file is already closed.
   *
   * @returns `Result<void>::ok()` on success, or an error code on failure.
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

  // ─── Accessors ──────────────────────────────────────────────────────────

  /**
   * @brief Returns the internal file descriptor.
   * @returns The file fd, or -1 if invalid.
   */
  [[nodiscard]] int fd() const noexcept { return fd_; }

  /**
   * @brief Checks whether the file is open.
   * @returns true if fd >= 0.
   */
  [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }

private:
  /** @brief The managed file descriptor. */
  int fd_;
};

} // namespace qbuem

/** @} */ // end of qbuem_io_buffers
