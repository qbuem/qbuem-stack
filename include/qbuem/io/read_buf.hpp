#pragma once

/**
 * @file qbuem/io/read_buf.hpp
 * @brief 소켓 수신용 컴파일 타임 고정 링 버퍼.
 * @ingroup qbuem_io_buffers
 *
 * `ReadBuf<N>`은 N바이트 크기의 링 버퍼를 스택에 보관합니다.
 * 힙 할당 없이 소켓 읽기 버퍼로 사용할 수 있으며,
 * 캐시 라인 정렬(`alignas(64)`)을 통해 false sharing을 방지합니다.
 *
 * ### 설계 원칙
 * - N은 컴파일 타임 상수 — 스택에 고정 크기 배열 할당
 * - `write_head()` → `commit()` → `readable()` → `consume()` 사이클
 * - `compact()`로 write_pos가 끝에 도달했을 때 잔여 데이터를 앞으로 이동
 * @{
 */

#include <qbuem/common.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>

namespace qbuem {

/**
 * @brief 소켓 수신용 컴파일 타임 고정 크기 링 버퍼.
 *
 * 내부 저장소가 캐시 라인(`alignas(64)`)에 정렬되어 있으며
 * 힙 할당이 전혀 발생하지 않습니다.
 *
 * ### 기본 사용 패턴
 * @code
 * ReadBuf<4096> buf;
 * // 1. 소켓에서 읽기
 * ssize_t n = ::read(fd, buf.write_head(), buf.writable_size());
 * if (n > 0) buf.commit(static_cast<size_t>(n));
 * // 2. 데이터 소비
 * auto view = buf.readable();   // span<const uint8_t>
 * // ... view 파싱 ...
 * buf.consume(parsed_bytes);
 * // 3. 공간 부족 시 compact
 * if (buf.writable_size() < 512) buf.compact();
 * @endcode
 *
 * @tparam N 버퍼 총 크기 (바이트). 0보다 커야 합니다.
 */
template <size_t N>
struct ReadBuf {
  static_assert(N > 0, "ReadBuf 크기는 0보다 커야 합니다");

  /**
   * @brief 내부 저장소.
   *
   * 캐시 라인(64바이트)에 정렬되어 false sharing과 SIMD 로드 패널티를 방지합니다.
   */
  alignas(64) std::byte storage[N];

  /** @brief 다음 쓰기 위치 (소켓에서 읽은 데이터를 저장할 오프셋). */
  size_t write_pos = 0;

  /** @brief 다음 읽기 위치 (애플리케이션이 소비할 데이터의 시작 오프셋). */
  size_t read_pos = 0;

  // ─── 쓰기 인터페이스 ─────────────────────────────────────────────────────

  /**
   * @brief 소켓에서 직접 읽을 쓰기 헤드 포인터를 반환합니다.
   *
   * @returns `storage + write_pos` — 아직 사용되지 않은 공간의 시작.
   * @warning `writable_size() > 0`인지 확인한 후 사용하세요.
   */
  [[nodiscard]] std::byte *write_head() noexcept {
    return storage + write_pos;
  }

  /**
   * @brief 현재 쓸 수 있는 공간(바이트)을 반환합니다.
   *
   * `N - write_pos`입니다. `compact()` 호출 후 값이 증가합니다.
   *
   * @returns 쓰기 가능한 바이트 수.
   */
  [[nodiscard]] size_t writable_size() const noexcept {
    return N - write_pos;
  }

  /**
   * @brief n바이트를 쓰기 완료로 표시합니다 (write_pos 전진).
   *
   * @param n 소켓에서 방금 읽은 바이트 수.
   * @pre `n <= writable_size()`.
   */
  void commit(size_t n) noexcept {
    assert(n <= writable_size() && "ReadBuf::commit 범위 초과");
    write_pos += n;
  }

  // ─── 읽기 인터페이스 ─────────────────────────────────────────────────────

  /**
   * @brief 소비 가능한 데이터 뷰를 반환합니다.
   *
   * @returns `[read_pos, write_pos)` 범위의 읽기 전용 뷰.
   */
  [[nodiscard]] BufferView readable() const noexcept {
    return {reinterpret_cast<const uint8_t *>(storage + read_pos),
            write_pos - read_pos};
  }

  /**
   * @brief n바이트를 소비 완료로 표시합니다 (read_pos 전진).
   *
   * @param n 애플리케이션이 방금 소비한 바이트 수.
   * @pre `n <= size()`.
   */
  void consume(size_t n) noexcept {
    assert(n <= size() && "ReadBuf::consume 범위 초과");
    read_pos += n;
  }

  // ─── 상태 및 유틸리티 ────────────────────────────────────────────────────

  /**
   * @brief 버퍼에 쌓인 소비 대기 중인 바이트 수를 반환합니다.
   *
   * @returns `write_pos - read_pos`.
   */
  [[nodiscard]] size_t size() const noexcept {
    return write_pos - read_pos;
  }

  /**
   * @brief 소비 대기 중인 데이터가 없는지 확인합니다.
   *
   * @returns `size() == 0`이면 true.
   */
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }

  /**
   * @brief 버퍼가 가득 찼는지 확인합니다 (쓰기 공간 없음).
   *
   * @returns `writable_size() == 0`이면 true.
   */
  [[nodiscard]] bool full() const noexcept { return writable_size() == 0; }

  /**
   * @brief 잔여 데이터를 버퍼 앞으로 이동하여 쓰기 공간을 확보합니다.
   *
   * `write_pos == N`이고 아직 소비되지 않은 데이터가 있을 때 호출합니다.
   * 이동 후 `read_pos = 0`, `write_pos = size()` 로 재설정됩니다.
   * 이미 read_pos가 0이면 no-op입니다.
   */
  void compact() noexcept {
    if (read_pos == 0)
      return;
    size_t remaining = size();
    if (remaining > 0)
      std::memmove(storage, storage + read_pos, remaining);
    write_pos = remaining;
    read_pos  = 0;
  }

  /**
   * @brief 버퍼를 완전히 비웁니다 (읽기/쓰기 위치를 0으로 초기화).
   */
  void reset() noexcept {
    read_pos  = 0;
    write_pos = 0;
  }
};

} // namespace qbuem

/** @} */ // end of qbuem_io_buffers
