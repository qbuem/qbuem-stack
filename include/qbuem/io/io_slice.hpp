#pragma once

/**
 * @file qbuem/io/io_slice.hpp
 * @brief 힙 할당 없는 I/O 버퍼 슬라이스 기본 타입.
 * @defgroup qbuem_io_buffers IO Buffer Primitives
 * @ingroup qbuem_io
 *
 * `IOSlice`와 `MutableIOSlice`는 연속된 바이트 영역을 가리키는 팻 포인터입니다.
 * 스택에 위치하며 힙 할당이 전혀 발생하지 않습니다.
 *
 * - `IOSlice`       : 읽기 전용 바이트 슬라이스 (BufferView / iovec 변환 지원)
 * - `MutableIOSlice`: 쓰기 가능 바이트 슬라이스 (MutableBufferView / iovec 변환 지원)
 *
 * ### 설계 원칙
 * - zero-alloc: 힙 할당 없음, 스택 값 타입
 * - BufferView / iovec 양방향 변환으로 기존 API와 자연스럽게 연동
 * @{
 */

#include <qbuem/common.hpp>

#include <cstddef>
#include <sys/uio.h>

namespace qbuem {

// ─── IOSlice ──────────────────────────────────────────────────────────────────

/**
 * @brief 읽기 전용 연속 바이트 영역을 가리키는 팻 포인터 (zero-alloc).
 *
 * 소유권을 갖지 않습니다. 슬라이스가 가리키는 버퍼의 수명이
 * 슬라이스보다 길어야 합니다.
 *
 * ### 사용 예시
 * @code
 * std::array<std::byte, 64> data{};
 * IOSlice slice{data.data(), data.size()};
 * auto bv = slice.to_buffer_view();   // span<const uint8_t>
 * auto iov = slice.to_iovec();        // struct iovec
 * @endcode
 */
struct IOSlice {
  /** @brief 버퍼의 첫 번째 바이트를 가리키는 포인터. */
  const std::byte *data;

  /** @brief 버퍼의 바이트 수. */
  size_t size;

  /**
   * @brief 읽기 전용 `BufferView`로 변환합니다.
   *
   * @returns `std::span<const uint8_t>`로 표현한 동일한 바이트 영역.
   * @note 포인터 타입만 재해석됩니다 — 복사는 없습니다.
   */
  [[nodiscard]] BufferView to_buffer_view() const noexcept {
    return {reinterpret_cast<const uint8_t *>(data), size};
  }

  /**
   * @brief POSIX `iovec` 구조체로 변환합니다.
   *
   * `readv(2)` / `writev(2)` 등 scatter-gather syscall에 직접 전달할 수 있습니다.
   * `iov_base`는 `const_cast`로 비-const 포인터로 변환됩니다 (POSIX 요구사항).
   *
   * @returns 동일한 바이트 영역을 가리키는 `iovec`.
   */
  [[nodiscard]] iovec to_iovec() const noexcept {
    return {const_cast<std::byte *>(data), size};
  }
};

// ─── MutableIOSlice ───────────────────────────────────────────────────────────

/**
 * @brief 쓰기 가능한 연속 바이트 영역을 가리키는 팻 포인터 (zero-alloc).
 *
 * 소유권을 갖지 않습니다. 슬라이스가 가리키는 버퍼의 수명이
 * 슬라이스보다 길어야 합니다.
 *
 * ### 사용 예시
 * @code
 * alignas(64) std::byte recv_buf[4096];
 * MutableIOSlice slice{recv_buf, sizeof(recv_buf)};
 * auto bv = slice.to_buffer_view();   // span<uint8_t>
 * auto iov = slice.to_iovec();        // struct iovec
 * @endcode
 */
struct MutableIOSlice {
  /** @brief 버퍼의 첫 번째 바이트를 가리키는 쓰기 가능 포인터. */
  std::byte *data;

  /** @brief 버퍼의 바이트 수. */
  size_t size;

  /**
   * @brief 쓰기 가능한 `MutableBufferView`로 변환합니다.
   *
   * @returns `std::span<uint8_t>`로 표현한 동일한 바이트 영역.
   * @note 포인터 타입만 재해석됩니다 — 복사는 없습니다.
   */
  [[nodiscard]] MutableBufferView to_buffer_view() const noexcept {
    return {reinterpret_cast<uint8_t *>(data), size};
  }

  /**
   * @brief POSIX `iovec` 구조체로 변환합니다.
   *
   * @returns 동일한 바이트 영역을 가리키는 `iovec`.
   */
  [[nodiscard]] iovec to_iovec() const noexcept {
    return {data, size};
  }

  /**
   * @brief 읽기 전용 `IOSlice`로 변환합니다.
   *
   * @returns 동일한 바이트 영역을 가리키는 읽기 전용 슬라이스.
   */
  [[nodiscard]] IOSlice as_const() const noexcept {
    return {data, size};
  }
};

} // namespace qbuem

/** @} */ // end of qbuem_io_buffers
