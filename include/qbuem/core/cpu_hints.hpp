#pragma once

/**
 * @file qbuem/core/cpu_hints.hpp
 * @brief CPU 성능 힌트 — Prefetch, Cache-line, 연결 구조체 선제 로드
 * @defgroup qbuem_cpu_hints CPU Hints
 * @ingroup qbuem_core
 *
 * 컴파일러와 CPU에 메모리 접근 패턴을 알려 캐시 미스를 줄입니다.
 *
 * ## 사용 예시
 * ```cpp
 * // 다음 연결 구조체를 미리 캐시에 로드
 * Connection* next = ring.peek_next();
 * qbuem::prefetch_read(next);
 *
 * // 쓰기 목적 prefetch (write buffer 선제 로드)
 * qbuem::prefetch_write(output_buf);
 *
 * // cache-line 정렬 구조체
 * struct alignas(qbuem::kCacheLineSize) ReactorState {
 *     std::atomic<int> state;
 * };
 * ```
 *
 * @{
 */

#include <cstddef>
#include <cstdint>

namespace qbuem {

// ---------------------------------------------------------------------------
// Cache line size
// ---------------------------------------------------------------------------

/**
 * @brief 일반적인 x86-64 / ARM64 캐시 라인 크기 (64바이트).
 *
 * `std::hardware_destructive_interference_size`가 있으면 사용하고,
 * 없으면 64를 기본값으로 사용합니다.
 */
#ifdef __cpp_lib_hardware_interference_size
#  include <new>
inline constexpr size_t kCacheLineSize =
    std::hardware_destructive_interference_size;
#else
inline constexpr size_t kCacheLineSize = 64;
#endif

// ---------------------------------------------------------------------------
// Prefetch 힌트
// ---------------------------------------------------------------------------

/**
 * @brief 읽기 목적 prefetch — 데이터를 캐시로 미리 로드합니다.
 *
 * `__builtin_prefetch(ptr, 0, locality)`를 사용합니다.
 *
 * @tparam Locality 캐시 지역성 힌트:
 *   - 0: no temporal locality (캐시에 잠깐만 유지)
 *   - 1: low temporal locality
 *   - 2: moderate temporal locality
 *   - 3: high temporal locality (기본값 — L1에 유지)
 * @param ptr  prefetch할 메모리 주소.
 */
template <int Locality = 3>
inline void prefetch_read(const void* ptr) noexcept {
  __builtin_prefetch(ptr, 0, Locality);
}

/**
 * @brief 쓰기 목적 prefetch — 쓰기 버퍼를 캐시로 미리 로드합니다.
 *
 * `__builtin_prefetch(ptr, 1, locality)`를 사용합니다.
 *
 * @tparam Locality 캐시 지역성 힌트 (prefetch_read와 동일).
 * @param ptr  prefetch할 메모리 주소.
 */
template <int Locality = 3>
inline void prefetch_write(void* ptr) noexcept {
  __builtin_prefetch(ptr, 1, Locality);
}

/**
 * @brief 연결 구조체 배열에서 다음 N개 항목을 prefetch합니다.
 *
 * Accept 루프 등에서 다음 연결 구조체를 미리 캐시에 로드할 때 사용합니다.
 *
 * ```cpp
 * for (int i = 0; i < n; ++i) {
 *     prefetch_ahead(connections, i, n);
 *     process(connections[i]);
 * }
 * ```
 *
 * @tparam T     연결 구조체 타입.
 * @tparam Ahead 몇 개 앞을 prefetch할지 (기본 4).
 * @param  arr   배열 포인터.
 * @param  cur   현재 인덱스.
 * @param  count 배열 전체 길이.
 */
template <typename T, int Ahead = 4>
inline void prefetch_ahead(const T* arr, size_t cur, size_t count) noexcept {
  size_t next = cur + static_cast<size_t>(Ahead);
  if (next < count) {
    prefetch_read<1>(&arr[next]);
  }
}

// ---------------------------------------------------------------------------
// 컴파일러 힌트 매크로
// ---------------------------------------------------------------------------

/**
 * @brief 조건이 참일 가능성이 높다고 컴파일러에 알립니다.
 */
#define QBUEM_LIKELY(x)   __builtin_expect(!!(x), 1)

/**
 * @brief 조건이 거짓일 가능성이 높다고 컴파일러에 알립니다.
 */
#define QBUEM_UNLIKELY(x) __builtin_expect(!!(x), 0)

/**
 * @brief 함수가 자주 호출되지 않음을 컴파일러에 알립니다 (cold path).
 */
#define QBUEM_COLD __attribute__((cold))

/**
 * @brief 함수가 자주 호출됨을 컴파일러에 알립니다 (hot path).
 */
#define QBUEM_HOT  __attribute__((hot))

// ---------------------------------------------------------------------------
// Cache-line 정렬 헬퍼
// ---------------------------------------------------------------------------

/**
 * @brief 두 원자적 변수를 다른 캐시 라인에 배치하여 false sharing을 방지합니다.
 *
 * ```cpp
 * struct Counter {
 *     alignas(kCacheLineSize) std::atomic<uint64_t> producer_count{0};
 *     alignas(kCacheLineSize) std::atomic<uint64_t> consumer_count{0};
 * };
 * ```
 */
struct CacheLinePad {
  char pad[kCacheLineSize];
};

// ---------------------------------------------------------------------------
// 메모리 배리어
// ---------------------------------------------------------------------------

/**
 * @brief 컴파일러 메모리 배리어 — 컴파일러 재배열만 방지합니다.
 *
 * CPU 레벨 재배열은 막지 않습니다. CPU 배리어가 필요하면
 * `std::atomic_thread_fence(std::memory_order_seq_cst)` 사용.
 */
inline void compiler_barrier() noexcept {
  asm volatile("" ::: "memory");
}

/**
 * @brief CPU pause 명령어 — spin-wait 루프에서 사용합니다.
 *
 * x86: `PAUSE`, ARM: `YIELD`, 기타: no-op.
 * 하이퍼스레딩 환경에서 스핀락 경쟁을 줄입니다.
 */
inline void cpu_pause() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  asm volatile("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
  asm volatile("yield" ::: "memory");
#else
  compiler_barrier();
#endif
}

} // namespace qbuem

/** @} */
