#pragma once

/**
 * @file draco/core/arena.hpp
 * @brief 고성능 메모리 관리용 Arena 할당자 및 FixedPoolResource 정의.
 * @ingroup qbuem_memory
 *
 * 이 헤더는 qbuem-stack의 두 가지 핵심 메모리 관리 전략을 제공합니다:
 *
 * 1. **Arena**: 가변 크기 객체를 위한 bump-pointer 할당자.
 *    - 할당: O(1) (포인터 증가만 수행)
 *    - 해제: O(1) (reset() 호출 시 일괄 해제, 개별 해제 없음)
 *    - 사용 사례: HTTP 요청 처리처럼 수명이 명확한 단기 객체들의 일괄 관리.
 *
 * 2. **FixedPoolResource**: 고정 크기 객체를 위한 free-list 풀 할당자.
 *    - 할당 및 해제: O(1) (연결 리스트 헤드 포인터 조작만 수행)
 *    - 사용 사례: 동일 크기의 객체(예: Connection, 코루틴 프레임)를 반복 할당/해제.
 *
 * ### 스레드 안전성
 * 두 클래스 모두 **스레드 안전하지 않습니다**. 이는 의도된 설계로,
 * Shared-Nothing 아키텍처에서 각 스레드/코어가 독립적인 할당자를 소유합니다.
 * 스레드 간 공유가 필요하다면 외부에서 동기화를 제공해야 합니다.
 */

/**
 * @defgroup qbuem_memory Memory Management
 * @brief Arena 및 고정 크기 풀 할당자.
 *
 * 전통적인 `new`/`delete` 대신 이 할당자를 사용하면:
 * - malloc의 락 경합을 피할 수 있습니다.
 * - 메모리 단편화를 크게 줄일 수 있습니다.
 * - 수명이 같은 객체들을 O(1)에 일괄 해제할 수 있습니다.
 * @{
 */

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

namespace draco {

/**
 * @brief 가변 크기 객체를 위한 bump-pointer Arena 할당자.
 *
 * 내부적으로 고정 크기 블록들의 목록을 관리합니다. 각 블록은
 * bump-pointer 방식으로 순차적으로 채워집니다. 현재 블록이 다 차면
 * 새 블록을 동적 할당합니다.
 *
 * ### O(1) 할당 전략
 * 할당 시 포인터 연산만 수행합니다:
 * 1. 정렬 패딩 계산 (비트 연산)
 * 2. `current_ptr_` 포인터 증가
 * 3. 블록 경계 초과 시 새 블록 할당 후 반복
 *
 * ### 수명 관리
 * Arena 내의 객체들은 소멸자가 호출되지 않습니다.
 * 소멸자가 있는 타입은 직접 호출하거나, 소멸자가 필요 없는 타입에만 사용하세요.
 *
 * ### 사용 예시
 * @code
 * draco::Arena arena(64 * 1024); // 64 KiB 초기 블록
 *
 * // Arena에서 메모리 할당
 * auto *buf = static_cast<uint8_t *>(arena.allocate(1024));
 *
 * // HTTP 요청 처리 완료 후 일괄 해제
 * arena.reset(); // OS에 메모리를 반환하지 않고 포인터만 초기화
 * @endcode
 *
 * @note Arena는 이동(move) 가능하지만 복사(copy)는 불가능합니다.
 *       각 스레드마다 독립적인 Arena를 생성하세요.
 * @warning 개별 객체의 해제(`free`)는 지원하지 않습니다.
 *          Arena 전체를 `reset()`하거나 Arena 객체 자체를 파괴해야 합니다.
 */
class Arena {
public:
  /**
   * @brief 지정한 초기 블록 크기로 Arena를 생성합니다.
   *
   * 생성 즉시 첫 번째 블록을 할당합니다. 이후 블록이 부족해지면
   * 자동으로 새 블록을 추가합니다.
   *
   * @param initial_size 첫 번째 블록의 크기 (바이트). 기본값 64 KiB.
   */
  explicit Arena(size_t initial_size = 64 * 1024)
      : current_block_size_(initial_size) {
    allocate_block(current_block_size_);
  }

  /** @brief 모든 소유 블록을 해제합니다. */
  ~Arena() = default;

  /** @brief 복사 생성 불가 — Arena는 소유권을 독점합니다. */
  Arena(const Arena &) = delete;
  /** @brief 복사 대입 불가 — Arena는 소유권을 독점합니다. */
  Arena &operator=(const Arena &) = delete;
  /** @brief 이동 생성 가능. 원본 Arena는 비어있는 상태가 됩니다. */
  Arena(Arena &&) = default;
  /** @brief 이동 대입 가능. 원본 Arena는 비어있는 상태가 됩니다. */
  Arena &operator=(Arena &&) = default;

  /**
   * @brief Arena에서 메모리를 할당합니다 (O(1) 평균).
   *
   * 현재 블록에 공간이 있으면 포인터를 증가시켜 즉시 반환합니다.
   * 공간이 부족하면 새 블록을 할당한 후 반환합니다.
   *
   * 정렬은 `alignment` 파라미터에 따라 자동으로 맞춰집니다.
   *
   * @param size      요청할 메모리 크기 (바이트).
   * @param alignment 요청할 메모리 정렬값. 기본값은 플랫폼 최대 정렬값.
   * @returns 할당된 메모리의 포인터. 절대 nullptr을 반환하지 않습니다.
   * @throws std::bad_alloc 시스템 메모리가 부족한 경우 (새 블록 할당 실패 시).
   *
   * @note 반환된 포인터의 수명은 Arena의 수명에 종속됩니다.
   *       Arena가 `reset()`되거나 파괴되면 이 포인터는 무효화됩니다.
   */
  void *allocate(size_t size, size_t alignment = alignof(std::max_align_t)) {
    size_t padding =
        (alignment - (reinterpret_cast<uintptr_t>(current_ptr_) % alignment)) %
        alignment;
    size_t total_size = size + padding;

    if (current_ptr_ + total_size > current_block_end_) {
      // Need a new block
      allocate_block(std::max(size * 2, current_block_size_));
      padding = (alignment -
                 (reinterpret_cast<uintptr_t>(current_ptr_) % alignment)) %
                alignment;
      total_size = size + padding;
    }

    void *result = current_ptr_ + padding;
    current_ptr_ += total_size;
    return result;
  }

  /**
   * @brief Arena를 초기화하여 모든 메모리를 재사용 가능하게 만듭니다 (O(1)).
   *
   * 실제로 OS에 메모리를 반환하지 않습니다. 내부 bump-pointer만 첫 번째
   * 블록의 시작으로 재설정합니다. 이전에 할당된 모든 포인터는 무효화됩니다.
   *
   * HTTP 요청/응답 처리 사이클처럼 반복적인 단기 할당 패턴에서
   * OS 메모리 반환 비용 없이 메모리를 재활용할 수 있어 효율적입니다.
   *
   * @warning reset() 이후에는 이전에 Arena에서 할당한 모든 포인터가
   *          사용 불가능해집니다. 댕글링 포인터 사용에 주의하세요.
   */
  void reset() {
    if (blocks_.empty())
      return;

    // Keep only the largest block to avoid fragmentation over time,
    // or just reset all to the start of the first block.
    // For simplicity, we'll reset to the start of the first block and keep
    // others for reuse.
    current_block_index_ = 0;
    setup_block(0);
  }

private:
  /**
   * @brief 새 메모리 블록을 할당하고 현재 블록으로 설정합니다.
   * @param size 새 블록의 크기 (바이트).
   */
  void allocate_block(size_t size) {
    auto block = std::make_unique<uint8_t[]>(size);
    uint8_t *ptr = block.get();
    blocks_.push_back(std::move(block));
    block_ends_.push_back(ptr + size);

    current_block_index_ = blocks_.size() - 1;
    current_ptr_ = ptr;
    current_block_end_ = block_ends_.back();
  }

  /**
   * @brief 지정한 인덱스의 블록을 현재 블록으로 설정합니다.
   * @param index 활성화할 블록의 인덱스 (blocks_ 벡터 기준).
   */
  void setup_block(size_t index) {
    current_block_index_ = index;
    current_ptr_ = blocks_[index].get();
    current_block_end_ = block_ends_[index];
  }

  /** @brief 최초 블록 크기 및 새 블록 할당 기준 크기 (바이트). */
  size_t current_block_size_;

  /** @brief 소유한 메모리 블록들의 목록. unique_ptr로 수명을 관리합니다. */
  std::vector<std::unique_ptr<uint8_t[]>> blocks_;

  /** @brief 각 블록의 끝 포인터. `blocks_`와 1:1 대응합니다. */
  std::vector<uint8_t *> block_ends_;

  /** @brief 현재 활성 블록의 인덱스 (blocks_ 기준). */
  size_t current_block_index_ = 0;

  /**
   * @brief 현재 블록 내의 bump-pointer.
   *
   * 다음 할당이 시작될 위치를 가리킵니다.
   * 할당할 때마다 이 포인터가 앞으로 이동합니다.
   */
  uint8_t *current_ptr_ = nullptr;

  /** @brief 현재 블록의 끝 포인터. 이 이상으로 할당할 수 없습니다. */
  uint8_t *current_block_end_ = nullptr;
};

// ─── FixedPoolResource ────────────────────────────────────────────────────────

/**
 * @brief 동일 크기 객체를 위한 O(1) 고정 크기 풀 할당자.
 *
 * 슬롯 내부에 free-list 포인터를 임베드하는 방식으로 구현됩니다.
 * 별도의 메타데이터 저장소 없이 각 미사용 슬롯의 첫 bytes를
 * 다음 자유 슬롯에 대한 포인터로 활용합니다.
 *
 * ### 동작 원리
 * 초기화 시:
 * ```
 * [slot0] -> [slot1] -> [slot2] -> ... -> [slotN-1] -> nullptr
 * free_list_ = slot0
 * ```
 * allocate() 시: `free_list_`에서 헤드를 꺼냄 (O(1))
 * deallocate() 시: 반환된 슬롯을 `free_list_` 헤드에 추가 (O(1))
 *
 * ### 사용 예시
 * @code
 * // sizeof(MyCtx) 크기의 슬롯 256개를 가진 풀 생성
 * draco::FixedPoolResource<sizeof(MyCtx), alignof(MyCtx)> pool(256);
 *
 * void *slot = pool.allocate();   // O(1), 풀이 고갈되면 nullptr 반환
 * if (slot) {
 *     auto *ctx = new (slot) MyCtx(...); // placement new
 *     // ... 사용 ...
 *     ctx->~MyCtx();              // 소멸자 명시적 호출
 *     pool.deallocate(slot);      // O(1)
 * }
 * @endcode
 *
 * @tparam ObjectSize  각 슬롯의 오브젝트 크기 (바이트).
 *                     내부적으로 Alignment에 맞게 올림(round up)됩니다.
 * @tparam Alignment   슬롯의 메모리 정렬값. 기본값은 플랫폼 최대 정렬값.
 *
 * @note `ObjectSize`는 `sizeof(void*)`보다 커야 합니다.
 *       슬롯 내부에 free-list 포인터를 저장하기 때문입니다.
 * @note 스레드 안전하지 않습니다. 각 스레드마다 독립적인 풀을 사용하거나
 *       외부에서 동기화를 제공하세요.
 * @warning `deallocate()`에 풀에서 할당받지 않은 포인터를 전달하면
 *          정의되지 않은 동작(UB)이 발생합니다.
 */
template <size_t ObjectSize, size_t Alignment = alignof(std::max_align_t)>
class FixedPoolResource {
  /**
   * @brief 실제 슬롯 크기 (Alignment에 맞게 올림).
   *
   * 비트 마스킹으로 계산됩니다: `(ObjectSize + Alignment - 1) & ~(Alignment - 1)`
   */
  static constexpr size_t kSlotSize =
      (ObjectSize + Alignment - 1) & ~(Alignment - 1);

  static_assert(kSlotSize >= sizeof(void *),
                "ObjectSize must be >= sizeof(void*) to embed the free-list ptr");

  /**
   * @brief aligned new[]로 할당한 메모리의 커스텀 해제자.
   *
   * `std::align_val_t`로 할당한 메모리는 반드시 동일한 방식으로 해제해야
   * 정의되지 않은 동작을 피할 수 있습니다.
   */
  struct AlignedDeleter {
    void operator()(uint8_t *p) const noexcept {
      ::operator delete[](p, std::align_val_t{Alignment});
    }
  };

public:
  /**
   * @brief 지정한 용량으로 풀을 생성하고 free-list를 초기화합니다.
   *
   * 생성 시 `capacity * kSlotSize` 바이트를 한 번에 할당하고,
   * 각 슬롯을 연결 리스트로 구성합니다.
   *
   * @param capacity 풀의 최대 슬롯 수. 이 수를 초과하면 `allocate()`가 nullptr을 반환합니다.
   */
  explicit FixedPoolResource(size_t capacity)
      : capacity_(capacity),
        storage_(static_cast<uint8_t *>(
            ::operator new[](kSlotSize * capacity, std::align_val_t{Alignment}))) {
    // Build free list: each slot stores a pointer to the next slot.
    for (size_t i = 0; i + 1 < capacity_; ++i) {
      auto *slot = reinterpret_cast<void **>(storage_.get() + i * kSlotSize);
      *slot = storage_.get() + (i + 1) * kSlotSize;
    }
    auto *last = reinterpret_cast<void **>(
        storage_.get() + (capacity_ - 1) * kSlotSize);
    *last = nullptr;
    free_list_ = storage_.get();
  }

  /** @brief 모든 소유 메모리를 해제합니다. 이미 할당된 슬롯의 소멸자는 호출하지 않습니다. */
  ~FixedPoolResource() = default;

  /** @brief 복사 생성 불가 — 풀은 소유권을 독점합니다. */
  FixedPoolResource(const FixedPoolResource &) = delete;
  /** @brief 복사 대입 불가 — 풀은 소유권을 독점합니다. */
  FixedPoolResource &operator=(const FixedPoolResource &) = delete;
  /** @brief 이동 생성 가능. 원본 풀은 비어있는 상태가 됩니다. */
  FixedPoolResource(FixedPoolResource &&) = default;
  /** @brief 이동 대입 가능. 원본 풀은 비어있는 상태가 됩니다. */
  FixedPoolResource &operator=(FixedPoolResource &&) = default;

  /**
   * @brief 풀에서 슬롯 하나를 할당합니다 (O(1)).
   *
   * free-list의 헤드에서 슬롯을 꺼내어 반환합니다.
   * 반환된 슬롯은 초기화되지 않은 상태이므로 placement new 등으로
   * 직접 초기화해야 합니다.
   *
   * @returns 할당된 슬롯의 포인터. 풀이 고갈된 경우 nullptr 반환.
   * @note `[[nodiscard]]` 속성이 있어 반환값을 무시하면 컴파일 경고가 발생합니다.
   */
  [[nodiscard]] void *allocate() noexcept {
    if (!free_list_) [[unlikely]]
      return nullptr;
    void *slot = free_list_;
    free_list_ = *reinterpret_cast<void **>(free_list_);
    ++used_;
    return slot;
  }

  /**
   * @brief 슬롯을 풀에 반환합니다 (O(1)).
   *
   * 반환된 슬롯을 free-list의 헤드에 추가합니다.
   * 이 함수를 호출하기 전에 슬롯 내의 객체 소멸자를 명시적으로 호출해야 합니다.
   *
   * @param ptr `allocate()`로 얻은 슬롯 포인터.
   * @warning 풀에서 할당받지 않은 포인터를 전달하면 정의되지 않은 동작이 발생합니다.
   * @warning nullptr 전달 시 정의되지 않은 동작이 발생합니다.
   */
  void deallocate(void *ptr) noexcept {
    *reinterpret_cast<void **>(ptr) = free_list_;
    free_list_ = ptr;
    --used_;
  }

  /**
   * @brief 풀의 총 슬롯 수를 반환합니다.
   * @returns 생성 시 지정한 capacity 값.
   */
  size_t capacity()  const noexcept { return capacity_; }

  /**
   * @brief 현재 할당 중인 슬롯 수를 반환합니다.
   * @returns 현재 사용 중인 슬롯 수.
   */
  size_t used()      const noexcept { return used_; }

  /**
   * @brief 현재 사용 가능한 슬롯 수를 반환합니다.
   * @returns `capacity() - used()`.
   */
  size_t available() const noexcept { return capacity_ - used_; }

private:
  /** @brief 풀의 총 슬롯 수. */
  size_t   capacity_;

  /** @brief 현재 할당 중인 슬롯 수. */
  size_t   used_ = 0;

  /**
   * @brief free-list의 헤드 포인터.
   *
   * 각 자유 슬롯의 첫 bytes는 다음 자유 슬롯을 가리키는 포인터를 담습니다.
   * 마지막 자유 슬롯은 nullptr을 저장합니다.
   */
  void    *free_list_ = nullptr;

  /** @brief 정렬된 연속 메모리 블록. `capacity * kSlotSize` 바이트를 소유합니다. */
  std::unique_ptr<uint8_t[], AlignedDeleter> storage_;
};

} // namespace draco

/** @} */ // end of qbuem_memory
