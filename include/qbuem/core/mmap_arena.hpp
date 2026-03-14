#pragma once

/**
 * @file qbuem/core/mmap_arena.hpp
 * @brief mmap 기반 Arena 할당자 — madvise(MADV_DONTNEED/MADV_FREE) 지원.
 * @ingroup qbuem_memory
 *
 * 이 헤더는 mmap으로 할당한 고정 용량 메모리 영역 위에서 동작하는
 * bump-pointer Arena 할당자를 제공합니다.
 *
 * ## 기존 Arena(arena.hpp)와의 차이점
 * | 기능                  | Arena            | MmapArena                     |
 * |----------------------|-----------------|-------------------------------|
 * | 메모리 소스           | `new[]`          | `mmap(MAP_ANONYMOUS)`         |
 * | 용량                  | 동적 확장        | 생성 시 고정                  |
 * | reset() 동작          | 포인터만 초기화  | 포인터 초기화 + `MADV_DONTNEED` |
 * | 연결 종료 후 처리     | 없음             | `MADV_FREE` (lazy OS 반환)    |
 *
 * ## 설계 원칙
 * - **O(1) 할당**: bump-pointer 증가만 수행합니다.
 * - **제로 오버헤드 reset()**: 포인터를 시작점으로 되돌리고 `MADV_DONTNEED`로
 *   페이지를 즉시 OS에 반환합니다. 다음 접근 시 커널이 zero-fill 페이지를 제공합니다.
 * - **Lazy OS 반환**: `release()`는 `MADV_FREE`를 호출하여 커널이 여유 시간에
 *   물리 페이지를 회수하도록 힌트를 줍니다.
 *
 * ## 플랫폼 지원
 * - Linux: `mmap(MAP_ANONYMOUS)`, `madvise(MADV_DONTNEED)`, `madvise(MADV_FREE)`.
 * - 비Linux: `new std::byte[]` / `delete[]` 폴백, madvise 호출은 no-op.
 *
 * ## 사용 예시
 * @code
 * // 4 MiB Arena 생성
 * qbuem::MmapArena arena(4 * 1024 * 1024);
 *
 * // 정렬된 메모리 할당 (bump-pointer)
 * void *hdr = arena.allocate(sizeof(MyHeader), alignof(MyHeader));
 * void *buf = arena.allocate(1024);
 *
 * // 요청 처리 완료 후: 포인터 초기화 + MADV_DONTNEED
 * arena.reset();
 *
 * // 연결 종료 시: MADV_FREE로 lazy 물리 메모리 반환
 * arena.release();
 * @endcode
 *
 * @{
 */

#include <cstddef>
#include <memory>
#include <new>

#if defined(__linux__)
#  include <sys/mman.h>
#  include <numaif.h>
#  if !defined(MPOL_BIND)
// numaif.h가 없는 경우 직접 정의 (graceful fallback)
#    define MPOL_BIND 2
extern "C" long mbind(void*, unsigned long, int, const unsigned long*,
                      unsigned long, unsigned int);
#  endif
#endif

namespace qbuem {

/**
 * @brief mmap 기반 bump-pointer Arena 할당자.
 *
 * 생성 시 지정한 `capacity_bytes` 크기의 mmap 영역을 확보합니다.
 * 이후 `allocate()`는 정렬 패딩을 포함한 bump-pointer 이동만 수행합니다.
 *
 * ### 정렬 처리
 * `allocate(bytes, align)` 내부에서 현재 포인터를 `align`의 배수로
 * 올림(round-up)한 뒤 `bytes`를 추가합니다:
 * ```
 * padding = (align - (current % align)) % align
 * result  = base + offset + padding
 * offset += padding + bytes
 * ```
 *
 * ### reset() vs release()
 * - `reset()`: 요청/응답 처리 사이클마다 호출. bump-pointer를 0으로 되돌리고
 *   `MADV_DONTNEED`로 물리 페이지를 즉시 회수합니다.
 * - `release()`: 연결 종료 시 호출. `MADV_FREE`로 커널에 lazy 회수 힌트를 줍니다.
 *               포인터는 초기화하지 않습니다.
 *
 * @note 복사 불가, 이동 가능.
 * @note 이 클래스는 스레드 안전하지 않습니다.
 *       각 스레드/코루틴이 독립적인 `MmapArena`를 소유하도록 설계하세요.
 * @warning `allocate()`가 반환한 포인터의 수명은 `MmapArena`의 수명 및
 *          `reset()` 호출에 종속됩니다.
 */
class MmapArena {
public:
  /**
   * @brief 지정한 용량으로 MmapArena를 생성합니다.
   *
   * `mmap(MAP_PRIVATE|MAP_ANONYMOUS)`으로 `capacity_bytes` 크기의
   * 가상 주소 공간을 예약합니다. 물리 페이지는 실제 접근 시 커밋됩니다.
   *
   * 비Linux 환경에서는 `new std::byte[capacity_bytes]`로 폴백합니다.
   *
   * @param capacity_bytes Arena의 최대 용량 (바이트). 0보다 커야 합니다.
   * @throws std::bad_alloc mmap 또는 new 할당이 실패한 경우.
   */
  explicit MmapArena(std::size_t capacity_bytes)
      : base_(map_memory(capacity_bytes)), capacity_(capacity_bytes) {}

  /**
   * @brief Arena를 파괴하고 mmap 영역을 해제합니다.
   *
   * Linux: `munmap`으로 전체 `capacity_` 바이트를 해제합니다.
   * 비Linux: `delete[]`로 해제합니다.
   */
  ~MmapArena() noexcept {
    if (base_ == nullptr) return;
#if defined(__linux__)
    ::munmap(base_, capacity_);
#else
    delete[] base_;
#endif
  }

  /** @brief 복사 생성 불가 — Arena는 소유권을 독점합니다. */
  MmapArena(const MmapArena &) = delete;
  /** @brief 복사 대입 불가 — Arena는 소유권을 독점합니다. */
  MmapArena &operator=(const MmapArena &) = delete;

  /**
   * @brief 이동 생성. 원본 Arena는 비어있는 상태(base_ == nullptr)가 됩니다.
   * @param other 이동할 원본 Arena.
   */
  MmapArena(MmapArena &&other) noexcept
      : base_(other.base_),
        capacity_(other.capacity_),
        offset_(other.offset_) {
    other.base_     = nullptr;
    other.capacity_ = 0;
    other.offset_   = 0;
  }

  /**
   * @brief 이동 대입. 기존 리소스를 해제하고 원본을 이어받습니다.
   * @param other 이동할 원본 Arena.
   * @returns `*this`.
   */
  MmapArena &operator=(MmapArena &&other) noexcept {
    if (this == &other) return *this;
    // 기존 리소스 해제
    if (base_ != nullptr) {
#if defined(__linux__)
      ::munmap(base_, capacity_);
#else
      delete[] base_;
#endif
    }
    base_     = other.base_;
    capacity_ = other.capacity_;
    offset_   = other.offset_;
    other.base_     = nullptr;
    other.capacity_ = 0;
    other.offset_   = 0;
    return *this;
  }

  /**
   * @brief Arena에서 정렬된 메모리를 할당합니다 (O(1) bump-pointer).
   *
   * 현재 bump-pointer를 `align` 경계로 올림한 후 `bytes`만큼 전진합니다.
   * 용량을 초과하면 `nullptr`을 반환합니다.
   *
   * @param bytes 요청할 메모리 크기 (바이트). 0이면 nullptr 반환.
   * @param align 메모리 정렬값 (2의 거듭제곱이어야 함).
   *              기본값은 `alignof(std::max_align_t)`.
   * @returns 할당된 메모리 포인터. 용량 초과 시 `nullptr`.
   *
   * @note 반환된 포인터의 수명은 `MmapArena` 객체 및 `reset()` 호출에 종속됩니다.
   */
  [[nodiscard]] void *allocate(
      std::size_t bytes,
      std::size_t align = alignof(std::max_align_t)) noexcept {
    if (bytes == 0 || base_ == nullptr) return nullptr;

    // 현재 포인터를 align의 배수로 올림합니다.
    auto current = reinterpret_cast<std::uintptr_t>(base_) + offset_;
    std::size_t padding = (align - (current % align)) % align;

    // 용량 초과 검사
    if (offset_ + padding + bytes > capacity_) return nullptr;

    void *result = reinterpret_cast<void *>(current + padding);
    offset_ += padding + bytes;
    return result;
  }

  /**
   * @brief bump-pointer를 시작점으로 초기화하고 페이지를 OS에 반환합니다.
   *
   * offset을 0으로 되돌리므로 이전에 할당된 모든 포인터는 즉시 무효화됩니다.
   *
   * Linux에서는 `madvise(MADV_DONTNEED)`를 호출하여 전체 mmap 영역의
   * 물리 페이지를 OS에 즉시 반환합니다. 이후 접근 시 커널이
   * zero-fill 새 페이지를 제공합니다.
   *
   * 비Linux 환경에서는 포인터 초기화만 수행합니다.
   *
   * @note HTTP 요청/응답 처리 사이클 사이에 호출하면 메모리 재사용 효율이 높습니다.
   * @warning reset() 이후 이전에 할당한 모든 포인터는 사용 불가합니다.
   */
  void reset() noexcept {
    offset_ = 0;
#if defined(__linux__)
    if (base_ != nullptr) {
      ::madvise(base_, capacity_, MADV_DONTNEED);
    }
#endif
  }

  /**
   * @brief mmap 영역에 MADV_FREE 힌트를 주어 물리 페이지를 lazy하게 반환합니다.
   *
   * 커널이 여유 시간에 물리 페이지를 회수하도록 힌트를 줍니다.
   * 가상 주소 공간은 유지되며, 이후 접근 시 커널이 새 페이지를 제공합니다.
   * bump-pointer(`offset_`)는 초기화하지 않습니다.
   *
   * 비Linux 환경에서는 아무 동작도 하지 않습니다.
   *
   * @note 연결 종료(connection close) 시 호출하여 물리 메모리 pressure를 줄이세요.
   * @note `MADV_FREE`는 Linux 4.5 이상에서 지원됩니다.
   *       구형 커널에서는 EINVAL이 반환될 수 있으며, 이 함수는 오류를 무시합니다.
   */
  void release() noexcept {
#if defined(__linux__)
    if (base_ != nullptr) {
      ::madvise(base_, capacity_, MADV_FREE);
    }
#endif
  }

  /**
   * @brief 현재까지 할당된 바이트 수를 반환합니다.
   *
   * 정렬 패딩 바이트도 포함됩니다.
   *
   * @returns 사용 중인 바이트 수 (0 ~ capacity()).
   */
  [[nodiscard]] std::size_t used() const noexcept {
    return offset_;
  }

  /**
   * @brief Arena의 총 용량(바이트)을 반환합니다.
   *
   * 생성자에 전달한 `capacity_bytes`와 동일합니다.
   *
   * @returns mmap으로 할당한 전체 바이트 수.
   */
  [[nodiscard]] std::size_t capacity() const noexcept {
    return capacity_;
  }

  /**
   * @brief mmap 영역을 특정 NUMA 노드의 메모리에 바인딩합니다.
   *
   * `mbind(2)` (Linux)를 사용하여 이 Arena의 물리 페이지를 지정한 NUMA 노드에서
   * 할당하도록 정책을 설정합니다 (MPOL_BIND).
   *
   * reactor-local Arena를 같은 NUMA 노드 메모리에서 할당하면 크로스-NUMA
   * 메모리 접근을 방지하고 레이턴시를 줄입니다.
   *
   * @param numa_node  바인딩할 NUMA 노드 번호 (0-based).
   * @returns 성공 시 true. 비Linux 또는 libnuma 미설치 환경에서는 false.
   */
  bool bind_to_numa_node(int numa_node) noexcept {
#if defined(__linux__)
    if (base_ == nullptr || numa_node < 0) return false;

    // nodemask: 64비트 배열, 노드 번호에 해당하는 비트 설정
    unsigned long nodemask = 1UL << static_cast<unsigned>(numa_node);
    unsigned long maxnode  = static_cast<unsigned>(numa_node) + 1;

    int ret = static_cast<int>(::mbind(
        base_, capacity_,
        MPOL_BIND,
        &nodemask, maxnode + 1,
        0  // flags: 0 = 이후 할당에만 적용
    ));
    return (ret == 0);
#else
    (void)numa_node;
    return false;
#endif
  }

private:
  /**
   * @brief `size` 바이트의 메모리를 플랫폼에 맞게 할당합니다.
   *
   * Linux: `mmap(PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS)`.
   * 비Linux: `new std::byte[size]`.
   *
   * @param size 할당할 바이트 수.
   * @returns 할당된 메모리 포인터 (`void*` 형태이며 호출 측에서 캐스팅).
   * @throws std::bad_alloc 할당 실패 시.
   */
  static void *map_memory(std::size_t size) {
#if defined(__linux__)
    void *ptr = ::mmap(
        nullptr,
        size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0);

    if (ptr == MAP_FAILED) {
      throw std::bad_alloc{};
    }
    return ptr;
#else
    return new std::byte[size];
#endif
  }

  /** @brief mmap(또는 new[])으로 할당한 메모리의 시작 포인터. */
  void *base_ = nullptr;

  /** @brief mmap 영역의 총 바이트 수. */
  std::size_t capacity_ = 0;

  /**
   * @brief 현재 bump-pointer의 `base_`로부터의 오프셋 (바이트).
   *
   * `allocate()` 호출 시 정렬 패딩 + 요청 크기만큼 증가합니다.
   * `reset()` 시 0으로 초기화됩니다.
   */
  std::size_t offset_ = 0;
};

} // namespace qbuem

/** @} */
