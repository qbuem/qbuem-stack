#pragma once

/**
 * @file qbuem/core/huge_pages.hpp
 * @brief mmap(MAP_HUGETLB) 기반 huge page 버퍼 풀 정의.
 * @ingroup qbuem_memory
 *
 * 이 헤더는 huge page를 활용하여 TLB 미스를 최소화하는 고성능 버퍼 풀을 제공합니다.
 *
 * ## 설계 원칙
 * - `MAP_HUGETLB`로 huge page(2 MiB 또는 1 GiB) 매핑을 시도합니다.
 * - `ENOMEM` 또는 `EPERM` 오류 시 일반 `mmap(MAP_ANONYMOUS)`로 자동 폴백합니다.
 * - free-list 방식으로 버퍼를 관리하여 O(1) 획득/반환을 보장합니다.
 * - `std::mutex`로 스레드 안전성을 제공합니다.
 *
 * ## 플랫폼 지원
 * - Linux: `mmap(MAP_HUGETLB)` 또는 `mmap(MAP_ANONYMOUS)` 폴백.
 * - 비Linux: `new std::byte[N * Count]` 폴백.
 *
 * ## 사용 예시
 * @code
 * // 2 MiB 버퍼 8개를 가진 풀 생성
 * qbuem::HugeBufferPool<2 * 1024 * 1024, 8> pool;
 *
 * auto buf = pool.acquire(); // std::span<std::byte> 반환
 * if (!buf.empty()) {
 *     // 버퍼 사용
 *     pool.release(buf);     // 반환
 * }
 * @endcode
 *
 * @{
 */

#include <cstddef>
#include <mutex>
#include <span>
#include <vector>

#if defined(__linux__)
#  include <cerrno>
#  include <sys/mman.h>
#endif

namespace qbuem {

/**
 * @brief mmap(MAP_HUGETLB) 기반 huge page 버퍼 풀.
 *
 * 고정 크기 버퍼 `Count`개를 단일 huge page mmap으로 할당하고
 * free-list로 관리합니다. 버퍼는 `acquire()`로 빌리고 `release()`로 반납합니다.
 *
 * ### 메모리 레이아웃
 * 전체 `N * Count` 바이트를 하나의 연속된 mmap 영역으로 할당합니다.
 * 각 버퍼 슬롯은 `N` 바이트 단위로 정렬되어 있습니다:
 * ```
 * [버퍼 0 | 버퍼 1 | ... | 버퍼 Count-1]
 * ← N bytes →← N bytes →
 * ```
 *
 * ### Huge Page 폴백 전략
 * 1. `mmap(MAP_HUGETLB)` 시도.
 * 2. `errno == ENOMEM` 또는 `errno == EPERM`이면 일반 `mmap(MAP_ANONYMOUS)` 재시도.
 * 3. Linux 외 플랫폼에서는 `new std::byte[]`로 할당.
 *
 * ### 스레드 안전성
 * `acquire()`와 `release()`는 내부 `std::mutex`로 보호됩니다.
 *
 * @tparam N     버퍼 하나의 크기 (바이트). 0보다 커야 합니다.
 * @tparam Count 버퍼의 총 개수. 0보다 커야 합니다.
 *
 * @note 복사 불가, 이동 불가. 풀은 고정된 수명을 가집니다.
 * @warning `release()`에 풀에서 획득하지 않은 span을 전달하면 정의되지 않은 동작이 발생합니다.
 */
template <std::size_t N, std::size_t Count>
class HugeBufferPool {
  static_assert(N > 0,     "버퍼 크기 N은 0보다 커야 합니다.");
  static_assert(Count > 0, "버퍼 수 Count는 0보다 커야 합니다.");

public:
  /**
   * @brief 풀을 생성하고 mmap으로 메모리를 할당합니다.
   *
   * Linux에서는 우선 `MAP_HUGETLB`로 huge page 할당을 시도하며,
   * 실패 시 일반 anonymous mmap으로 폴백합니다.
   * 비Linux 환경에서는 `new std::byte[]`를 사용합니다.
   *
   * 초기화 후 free-list에는 모든 `Count`개의 버퍼가 등록됩니다.
   *
   * @throws std::bad_alloc mmap 또는 new 할당이 완전히 실패한 경우.
   */
  HugeBufferPool() {
    base_ = map_memory();
    // 모든 버퍼 슬롯을 free-list에 추가합니다.
    free_list_.reserve(Count);
    for (std::size_t i = 0; i < Count; ++i) {
      free_list_.push_back(base_ + i * N);
    }
  }

  /**
   * @brief 풀을 파괴하고 mmap 영역을 해제합니다.
   *
   * Linux에서는 `munmap`으로 전체 `N * Count` 바이트를 해제합니다.
   * 비Linux 환경에서는 `delete[]`로 해제합니다.
   *
   * @warning 아직 반납되지 않은 버퍼가 있어도 전체 메모리가 해제됩니다.
   *          소멸자 호출 전에 모든 버퍼를 `release()`하는 것을 권장합니다.
   */
  ~HugeBufferPool() noexcept {
    if (base_ == nullptr) return;
#if defined(__linux__)
    ::munmap(base_, N * Count);
#else
    delete[] base_;
#endif
  }

  /** @brief 복사 생성 불가 — 풀은 소유권을 독점합니다. */
  HugeBufferPool(const HugeBufferPool &) = delete;
  /** @brief 복사 대입 불가 — 풀은 소유권을 독점합니다. */
  HugeBufferPool &operator=(const HugeBufferPool &) = delete;
  /** @brief 이동 생성 불가 — mutex와 포인터 상태가 복잡합니다. */
  HugeBufferPool(HugeBufferPool &&) = delete;
  /** @brief 이동 대입 불가 — mutex와 포인터 상태가 복잡합니다. */
  HugeBufferPool &operator=(HugeBufferPool &&) = delete;

  /**
   * @brief 풀에서 버퍼 하나를 획득합니다 (O(1), 스레드 안전).
   *
   * free-list에서 버퍼 슬롯 하나를 꺼내 `std::span<std::byte>`로 반환합니다.
   * 풀이 고갈된 경우 빈 span(`empty() == true`)을 반환합니다.
   *
   * @returns `N` 바이트 크기의 `std::span<std::byte>`.
   *          풀이 고갈된 경우 빈 span.
   *
   * @note 반환된 span의 수명은 풀 객체의 수명에 종속됩니다.
   *       풀이 파괴되면 반환된 span은 무효화됩니다.
   * @note `[[nodiscard]]` 속성이 있어 반환값을 무시하면 컴파일 경고가 발생합니다.
   */
  [[nodiscard]] std::span<std::byte> acquire() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (free_list_.empty()) {
      return {}; // 빈 span 반환 — 풀 고갈
    }
    std::byte *ptr = free_list_.back();
    free_list_.pop_back();
    return {ptr, N};
  }

  /**
   * @brief 획득했던 버퍼를 풀에 반납합니다 (O(1), 스레드 안전).
   *
   * `acquire()`로 얻은 span을 free-list에 다시 추가합니다.
   * 빈 span을 전달하면 아무 동작도 하지 않습니다.
   *
   * @param buf `acquire()`로 얻은 `std::span<std::byte>`.
   *             크기가 `N`이 아닌 span을 전달하면 정의되지 않은 동작이 발생합니다.
   *
   * @warning 이 풀에서 획득하지 않은 span을 전달하면 정의되지 않은 동작이 발생합니다.
   */
  void release(std::span<std::byte> buf) noexcept {
    if (buf.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    free_list_.push_back(buf.data());
  }

  /**
   * @brief 현재 사용 가능한 (free-list에 있는) 버퍼 수를 반환합니다.
   *
   * 반환값은 호출 시점의 스냅샷이며, 멀티스레드 환경에서는 즉시 무효화될 수 있습니다.
   *
   * @returns free-list에 남아 있는 버퍼 수 (0 ~ Count).
   */
  [[nodiscard]] std::size_t available() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return free_list_.size();
  }

  /**
   * @brief 풀의 총 버퍼 수를 반환합니다.
   *
   * 생성 시 지정한 템플릿 파라미터 `Count`와 동일합니다.
   *
   * @returns `Count`.
   */
  [[nodiscard]] static constexpr std::size_t capacity() noexcept {
    return Count;
  }

private:
  /**
   * @brief `N * Count` 바이트의 메모리를 플랫폼에 맞게 할당합니다.
   *
   * Linux: `mmap(MAP_HUGETLB)` → 실패 시 `mmap(MAP_ANONYMOUS)` 순으로 시도.
   * 비Linux: `new std::byte[N * Count]`.
   *
   * @returns 할당된 메모리의 시작 포인터 (`std::byte*`).
   * @throws std::bad_alloc 모든 할당 시도가 실패한 경우.
   */
  static std::byte *map_memory() {
    static constexpr std::size_t kTotalBytes = N * Count;

#if defined(__linux__)
    // 1차 시도: MAP_HUGETLB (huge page)
    void *ptr = ::mmap(
        nullptr,
        kTotalBytes,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
        -1,
        0);

    if (ptr != MAP_FAILED) {
      return static_cast<std::byte *>(ptr);
    }

    // ENOMEM 또는 EPERM이면 일반 anonymous mmap으로 폴백
    if (errno == ENOMEM || errno == EPERM) {
      ptr = ::mmap(
          nullptr,
          kTotalBytes,
          PROT_READ | PROT_WRITE,
          MAP_PRIVATE | MAP_ANONYMOUS,
          -1,
          0);

      if (ptr != MAP_FAILED) {
        return static_cast<std::byte *>(ptr);
      }
    }

    throw std::bad_alloc{};
#else
    // 비Linux: new[] 폴백
    return new std::byte[kTotalBytes];
#endif
  }

  /** @brief mmap(또는 new[])으로 할당한 메모리의 시작 포인터. */
  std::byte *base_ = nullptr;

  /**
   * @brief 사용 가능한 버퍼 슬롯의 포인터를 담는 free-list.
   *
   * 각 원소는 `base_ + i * N`을 가리킵니다.
   * `acquire()`는 마지막 원소를 꺼내고, `release()`는 끝에 추가합니다.
   */
  std::vector<std::byte *> free_list_;

  /** @brief `acquire()`와 `release()` 호출을 보호하는 뮤텍스. */
  mutable std::mutex mutex_;
};

} // namespace qbuem

/** @} */
