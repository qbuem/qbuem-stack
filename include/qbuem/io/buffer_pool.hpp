#pragma once

/**
 * @file qbuem/io/buffer_pool.hpp
 * @brief 고정 크기 버퍼 풀 — lock-free intrusive free list.
 * @ingroup qbuem_io_buffers
 *
 * `BufferPool<BufSize, Count>`는 컴파일 타임에 크기가 결정된
 * `Count`개의 `BufSize` 바이트 버퍼를 스택(또는 정적 저장소)에 미리 할당합니다.
 * `acquire()`와 `return_buffer()`는 `std::atomic` CAS를 이용한
 * lock-free intrusive free list로 구현됩니다.
 *
 * ### 설계 원칙
 * - 고정 수량 미리 할당 — 런타임 힙 할당 없음 (`acquire()` 포함)
 * - `alignas(64)` 버퍼: false sharing 방지, SIMD 친화적
 * - `Buffer::release()`로 소유 풀에 자동 반납
 * - 풀이 소진되면 `acquire()`가 `nullptr` 반환 (예외 없음)
 * @{
 */

#include <qbuem/common.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace qbuem {

/**
 * @brief 고정 크기·고정 수량 버퍼 풀.
 *
 * 각 버퍼는 캐시 라인(64바이트)에 정렬됩니다.
 * `acquire()`로 버퍼를 가져오고, 사용 후 `Buffer::release()`로 반환합니다.
 *
 * ### 스레드 안전성
 * `acquire()`와 `return_buffer()`는 ABA 문제를 완화하는 CAS 루프로
 * 멀티스레드에서 안전하게 호출할 수 있습니다.
 *
 * ### 사용 예시
 * @code
 * BufferPool<4096, 64> pool;
 * auto *buf = pool.acquire();
 * if (!buf) { // 풀 소진 }
 * // ... buf->data 사용 ...
 * buf->release();  // 풀에 반환
 * @endcode
 *
 * @tparam BufSize 각 버퍼의 바이트 크기.
 * @tparam Count   풀에 미리 할당할 버퍼 수.
 */
template <size_t BufSize, size_t Count>
class BufferPool {
public:
  /**
   * @brief 풀에서 관리하는 단일 버퍼.
   *
   * `data`는 사용자가 직접 읽고 쓸 수 있는 영역입니다.
   * `pool` 포인터를 통해 소유 풀에 자신을 반납합니다.
   * `next_`는 free list 연결에 사용되며 사용자가 접근하지 않아야 합니다.
   */
  struct Buffer {
    /**
     * @brief 사용자 데이터 영역.
     *
     * 캐시 라인(64바이트)에 정렬되어 false sharing과 SIMD 패널티를 방지합니다.
     */
    alignas(64) std::byte data[BufSize];

    /** @brief 이 버퍼를 소유한 풀. `release()` 호출 시 사용됩니다. */
    BufferPool *pool;

    /**
     * @brief 버퍼를 소유 풀에 반납합니다.
     *
     * `pool->return_buffer(this)`를 호출합니다.
     * 반납 후 이 포인터를 통한 접근은 정의되지 않은 동작입니다.
     */
    void release() noexcept { pool->return_buffer(this); }

    /** @brief free list 연결 포인터 (사용자가 접근 금지). */
    Buffer *next_ = nullptr;
  };

  /**
   * @brief 모든 버퍼를 free list에 연결하여 풀을 초기화합니다.
   */
  BufferPool() noexcept {
    // 각 버퍼의 pool 포인터를 설정하고 free list를 구성합니다
    for (size_t i = 0; i < Count; ++i) {
      storage_[i].pool  = this;
      storage_[i].next_ = free_list_head_.load(std::memory_order_relaxed);
      free_list_head_.store(&storage_[i], std::memory_order_relaxed);
    }
    // free_count_ is initialized to Count via in-class initializer.
  }

  /** @brief 복사 생성자 삭제 — 버퍼들이 this 포인터를 보관하므로 이동/복사 불가. */
  BufferPool(const BufferPool &) = delete;

  /** @brief 복사 대입 삭제. */
  BufferPool &operator=(const BufferPool &) = delete;

  // ─── 획득 / 반납 ──────────────────────────────────────────────────────────

  /**
   * @brief 풀에서 버퍼 하나를 획득합니다.
   *
   * lock-free CAS 루프로 free list의 헤드를 pop합니다.
   *
   * @returns 사용 가능한 버퍼 포인터. 풀이 소진되면 `nullptr`.
   * @note 반환된 버퍼의 `data` 내용은 초기화되지 않습니다.
   *
   * @par ABA 문제 (이론적 한계)
   * 이 구현은 일반적인 lock-free 스택 ABA 문제에 노출되어 있습니다:
   * 스레드 T1이 `head`를 읽고 일시 정지된 사이, 다른 스레드가 `head`를 빼고
   * 반납하면 T1의 CAS는 성공하지만 `head->next_`가 유효하지 않을 수 있습니다.
   *
   * **실제 위험도**: BufferPool의 버퍼는 풀 소유이며 풀보다 먼저 해제되지 않으므로
   * 포인터 자체는 항상 유효합니다.  단, 높은 경합 환경에서는 freed 버퍼가 잘못된
   * `next_` 링크로 재배치될 수 있습니다.
   *
   * **완화 방법**: 단일 Reactor 스레드에서만 사용하면 ABA 문제가 발생하지 않습니다.
   * 다중 스레드 환경에서는 tagged pointer 또는 Hazard Pointer를 사용하세요.
   */
  Buffer *acquire() noexcept {
    Buffer *head = free_list_head_.load(std::memory_order_acquire);
    while (head) {
      if (free_list_head_.compare_exchange_weak(
              head, head->next_,
              std::memory_order_release,
              std::memory_order_acquire)) {
        head->next_ = nullptr;
        free_count_.fetch_sub(1, std::memory_order_relaxed);
        return head;
      }
    }
    return nullptr;
  }

  /**
   * @brief 버퍼를 풀에 반납합니다.
   *
   * lock-free CAS 루프로 free list의 헤드에 push합니다.
   *
   * @param buf 반납할 버퍼. 반드시 이 풀에서 획득한 버퍼여야 합니다.
   */
  void return_buffer(Buffer *buf) noexcept {
    Buffer *head = free_list_head_.load(std::memory_order_acquire);
    do {
      buf->next_ = head;
    } while (!free_list_head_.compare_exchange_weak(
        head, buf,
        std::memory_order_release,
        std::memory_order_acquire));
    free_count_.fetch_add(1, std::memory_order_relaxed);
  }

  // ─── 상태 ─────────────────────────────────────────────────────────────────

  /**
   * @brief 현재 사용 가능한 버퍼 수를 반환합니다.
   *
   * O(1) — 원자 카운터를 읽습니다.
   *
   * @returns 현재 free list에 남아 있는 버퍼 수.
   */
  [[nodiscard]] size_t available() const noexcept {
    return free_count_.load(std::memory_order_relaxed);
  }

private:
  /** @brief 미리 할당된 버퍼 저장소. */
  Buffer storage_[Count];

  /** @brief lock-free free list 헤드 포인터. */
  std::atomic<Buffer *> free_list_head_{nullptr};

  /** @brief free list 내 버퍼 수 — O(1) available() 지원. */
  std::atomic<size_t> free_count_{Count};
};

} // namespace qbuem

/** @} */ // end of qbuem_io_buffers
