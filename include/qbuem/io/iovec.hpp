#pragma once

/**
 * @file qbuem/io/iovec.hpp
 * @brief 스택 할당 scatter-gather I/O 벡터 배열.
 * @ingroup qbuem_io_buffers
 *
 * `IOVec<N>`은 최대 N개의 `iovec`을 스택에 보관하는 컨테이너입니다.
 * 힙 할당 없이 `writev(2)` / `readv(2)` 에 전달할 벡터 배열을 구성합니다.
 *
 * ### 설계 원칙
 * - N은 컴파일 타임 상수 — 스택에 고정 크기 배열 할당
 * - zero-alloc: 힙 할당 전혀 없음
 * - `push()` 오버로드를 통해 BufferView와 raw 포인터 모두 지원
 * @{
 */

#include <qbuem/common.hpp>

#include <cassert>
#include <cstddef>
#include <span>
#include <sys/uio.h>

namespace qbuem {

/**
 * @brief 스택 할당 scatter-gather iovec 배열.
 *
 * 최대 N개의 `iovec` 엔트리를 보관합니다.
 * `writev(2)` / `readv(2)` syscall에 직접 전달할 수 있도록
 * `as_span()` 메서드로 span을 얻습니다.
 *
 * ### 사용 예시
 * @code
 * IOVec<4> vec;
 * vec.push(header.data(), header.size());
 * vec.push(body_view);
 * ::writev(fd, vec.as_span().data(), static_cast<int>(vec.as_span().size()));
 * @endcode
 *
 * @tparam N iovec 최대 엔트리 수. 컴파일 타임 상수.
 */
template <size_t N>
struct IOVec {
  /** @brief iovec 엔트리 배열. */
  iovec vecs[N];

  /** @brief 현재 유효한 엔트리 수. */
  size_t count = 0;

  // ─── 추가 ────────────────────────────────────────────────────────────────

  /**
   * @brief raw 포인터와 길이로 iovec 엔트리를 추가합니다.
   *
   * @param data 버퍼 포인터.
   * @param len  버퍼 바이트 수.
   * @pre `count < N` — 초과 시 assert로 중단.
   */
  void push(const void *data, size_t len) noexcept {
    assert(count < N && "IOVec capacity exceeded");
    vecs[count].iov_base = const_cast<void *>(data);
    vecs[count].iov_len  = len;
    ++count;
  }

  /**
   * @brief `BufferView`로 iovec 엔트리를 추가합니다.
   *
   * @param buf 읽기 전용 바이트 뷰.
   * @pre `count < N` — 초과 시 assert로 중단.
   */
  void push(BufferView buf) noexcept {
    push(buf.data(), buf.size());
  }

  /**
   * @brief `MutableBufferView`로 iovec 엔트리를 추가합니다.
   *
   * @param buf 쓰기 가능 바이트 뷰.
   * @pre `count < N` — 초과 시 assert로 중단.
   */
  void push(MutableBufferView buf) noexcept {
    push(buf.data(), buf.size());
  }

  // ─── 접근자 ──────────────────────────────────────────────────────────────

  /**
   * @brief 유효한 iovec 엔트리들의 `std::span`을 반환합니다.
   *
   * @returns `iovec[0..count)` 범위의 mutable span.
   */
  [[nodiscard]] std::span<iovec> as_span() noexcept {
    return {vecs, count};
  }

  /**
   * @brief 유효한 iovec 엔트리들의 const `std::span`을 반환합니다.
   *
   * @returns `iovec[0..count)` 범위의 const span.
   */
  [[nodiscard]] std::span<const iovec> as_const_span() const noexcept {
    return {vecs, count};
  }

  /**
   * @brief 모든 엔트리의 총 바이트 수를 계산합니다.
   *
   * @returns 모든 `iov_len`의 합계.
   */
  [[nodiscard]] size_t total_bytes() const noexcept {
    size_t total = 0;
    for (size_t i = 0; i < count; ++i)
      total += vecs[i].iov_len;
    return total;
  }

  /**
   * @brief 모든 엔트리를 제거하고 count를 0으로 초기화합니다.
   */
  void clear() noexcept { count = 0; }

  /**
   * @brief 배열이 비어 있는지 확인합니다.
   * @returns `count == 0`이면 true.
   */
  [[nodiscard]] bool empty() const noexcept { return count == 0; }

  /**
   * @brief 배열이 가득 찼는지 확인합니다.
   * @returns `count == N`이면 true.
   */
  [[nodiscard]] bool full() const noexcept { return count == N; }
};

} // namespace qbuem

/** @} */ // end of qbuem_io_buffers
