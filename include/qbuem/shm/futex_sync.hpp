#pragma once

/**
 * @file qbuem/shm/futex_sync.hpp
 * @brief Futex-uring 동기화 — `IORING_OP_FUTEX_WAIT/WAKE` 기반 IPC 동기화.
 * @defgroup qbuem_shm_futex FutexSync
 * @ingroup qbuem_shm
 *
 * ## 개요
 * Linux 6.7+에 추가된 `IORING_OP_FUTEX_WAIT`와 `IORING_OP_FUTEX_WAKE`를
 * io_uring SQE로 발행하여 **non-blocking 코루틴 futex 동기화**를 구현합니다.
 *
 * ## 기존 futex(2) 대비 장점
 * | | futex(2) 직접 | io_uring futex |
 * |--|---------------|----------------|
 * | 블로킹 | 스레드 블로킹 | co_await, 비블로킹 |
 * | 배치 | 1개씩 | SQE 배치 발행 |
 * | 혼합 | 별도 I/O 불가 | I/O와 혼합 가능 |
 *
 * ## 구성
 * - `FutexWord`: 공유 메모리 내 futex 원자 변수 (32-bit 또는 64-bit).
 * - `FutexSync`: wait/wake 비동기 헬퍼.
 * - `FutexMutex`: futex 기반 프로세스 간 뮤텍스 (RAII).
 * - `FutexSemaphore`: futex 기반 카운팅 세마포어.
 *
 * ## Fallback
 * io_uring FUTEX_WAIT 미지원 커널(< 6.7)에서는
 * `futex(2)` syscall + 스레드 폴링으로 폴백합니다.
 *
 * @code
 * // 공유 메모리에 FutexWord 배치
 * auto shm = SHMSegment::create("ctrl", 4096);
 * auto* fw = new (shm.base()) FutexWord(0);
 *
 * // 생산자 (wake)
 * fw->store(1);
 * FutexSync::wake(*fw, 1); // 1개 waiter 깨움
 *
 * // 소비자 (wait) — 코루틴 내
 * co_await FutexSync::wait(*fw, 0, reactor); // fw == 0이면 대기
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>

#include <atomic>
#include <cstdint>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace qbuem::shm {

// ─── FutexWord ────────────────────────────────────────────────────────────────

/**
 * @brief 공유 메모리에 배치할 futex 원자 변수.
 *
 * `std::atomic<uint32_t>`와 달리 PROCESS_SHARED 플래그로 동작합니다.
 * 반드시 공유 메모리 세그먼트 내에 배치해야 합니다.
 *
 * @note `FUTEX_PRIVATE_FLAG` 없이 커널에 전달하여 크로스-프로세스 futex를 사용합니다.
 */
struct FutexWord {
    alignas(4) std::atomic<uint32_t> value;

    explicit constexpr FutexWord(uint32_t init = 0) noexcept : value(init) {}

    [[nodiscard]] uint32_t load(std::memory_order mo = std::memory_order_seq_cst) const noexcept {
        return value.load(mo);
    }

    void store(uint32_t v, std::memory_order mo = std::memory_order_seq_cst) noexcept {
        value.store(v, mo);
    }

    bool compare_exchange(uint32_t& expected, uint32_t desired,
                          std::memory_order mo = std::memory_order_seq_cst) noexcept {
        return value.compare_exchange_strong(expected, desired, mo);
    }

    uint32_t fetch_add(uint32_t v,
                       std::memory_order mo = std::memory_order_seq_cst) noexcept {
        return value.fetch_add(v, mo);
    }

    uint32_t fetch_sub(uint32_t v,
                       std::memory_order mo = std::memory_order_seq_cst) noexcept {
        return value.fetch_sub(v, mo);
    }
};
static_assert(sizeof(FutexWord) == 4, "FutexWord must be exactly 4 bytes");
static_assert(alignof(FutexWord) == 4, "FutexWord must be 4-byte aligned");

// ─── FutexSync ───────────────────────────────────────────────────────────────

/**
 * @brief io_uring 기반 비동기 futex wait/wake.
 *
 * `io_uring IORING_OP_FUTEX_WAIT` SQE를 발행하여 코루틴이
 * `co_await`으로 futex를 기다립니다.
 */
class FutexSync {
public:
    /**
     * @brief Futex wait (co_await).
     *
     * `fw.value == expected`이면 wake 또는 타임아웃까지 대기합니다.
     * 값이 이미 다르면 즉시 반환합니다.
     *
     * ## 내부 구현
     * - io_uring 지원 환경: `IORING_OP_FUTEX_WAIT` SQE 발행.
     * - 폴백 환경: `syscall(SYS_futex, FUTEX_WAIT)` + 스레드 yield.
     *
     * @param fw       공유 메모리 내 FutexWord.
     * @param expected wait 중단 조건 (fw.value != expected이면 즉시 반환).
     * @param reactor  io_uring SQE를 발행할 Reactor.
     * @param timeout_ns 대기 타임아웃 (나노초, 0이면 무제한).
     */
    static Task<void> wait(FutexWord& fw, uint32_t expected,
                            Reactor& reactor, uint64_t timeout_ns = 0) noexcept;

    /**
     * @brief Futex wake — 지정한 개수의 waiter를 깨웁니다.
     *
     * `IORING_OP_FUTEX_WAKE` 또는 `SYS_futex FUTEX_WAKE`로 깨웁니다.
     * 이 함수는 논블로킹이며 코루틴 내외부에서 모두 호출 가능합니다.
     *
     * @param fw      공유 메모리 내 FutexWord.
     * @param count   깨울 최대 waiter 수 (INT_MAX이면 전체).
     * @returns 깨운 waiter 수.
     */
    static int wake(FutexWord& fw, uint32_t count = 1) noexcept;

    /**
     * @brief 모든 waiter를 깨웁니다.
     */
    static int wake_all(FutexWord& fw) noexcept {
        return wake(fw, static_cast<uint32_t>(-1));
    }

    /**
     * @brief io_uring FUTEX 지원 여부를 런타임에 확인합니다.
     *
     * 커널 6.7 미만에서는 false를 반환합니다.
     */
    [[nodiscard]] static bool has_uring_futex() noexcept;

private:
    // io_uring SQE 발행 내부 함수
    static Task<void> wait_uring(FutexWord& fw, uint32_t expected,
                                  Reactor& reactor, uint64_t timeout_ns) noexcept;

    // 폴백: syscall + 스레드 블로킹 (Reactor spawn으로 offload)
    static Task<void> wait_syscall(FutexWord& fw, uint32_t expected,
                                    Reactor& reactor, uint64_t timeout_ns) noexcept;

    static int wake_syscall(FutexWord& fw, uint32_t count) noexcept;
};

// ─── FutexMutex ──────────────────────────────────────────────────────────────

/**
 * @brief 프로세스 간 futex 뮤텍스 (RAII).
 *
 * SHM 내에 배치하여 여러 프로세스가 공유하는 뮤텍스입니다.
 * `co_await`으로 획득 대기합니다.
 *
 * ## 상태
 * - `0`: Unlocked
 * - `1`: Locked (waiter 없음)
 * - `2`: Locked with waiters
 *
 * @code
 * FutexMutex* mtx = new(shm.base()) FutexMutex();
 *
 * // 임계 구역
 * auto guard = co_await mtx->lock(reactor);
 * // 작업 수행 ...
 * // guard 소멸 → 자동 unlock
 * @endcode
 */
class FutexMutex {
public:
    FutexMutex() = default;

    FutexMutex(const FutexMutex&) = delete;
    FutexMutex& operator=(const FutexMutex&) = delete;

    // ── RAII Lock Guard ─────────────────────────────────────────────────

    struct LockGuard {
        FutexMutex* mtx{nullptr};
        explicit LockGuard(FutexMutex* m) noexcept : mtx(m) {}
        LockGuard(LockGuard&& o) noexcept : mtx(o.mtx) { o.mtx = nullptr; }
        ~LockGuard() { if (mtx) mtx->unlock(); }
        LockGuard(const LockGuard&) = delete;
        LockGuard& operator=(const LockGuard&) = delete;
    };

    /**
     * @brief 뮤텍스를 획득합니다 (co_await).
     *
     * 이미 잠겨 있으면 futex wait으로 대기합니다.
     *
     * @param reactor io_uring SQE 발행용.
     * @returns RAII LockGuard.
     */
    Task<LockGuard> lock(Reactor& reactor) noexcept;

    /**
     * @brief 논블로킹 trylock.
     *
     * @returns 성공이면 LockGuard, 실패이면 nullopt.
     */
    std::optional<LockGuard> try_lock() noexcept {
        uint32_t expected = 0;
        if (fw_.compare_exchange(expected, 1, std::memory_order_acquire))
            return LockGuard{this};
        return std::nullopt;
    }

    /**
     * @brief 뮤텍스를 해제합니다.
     */
    void unlock() noexcept {
        fw_.store(0, std::memory_order_release);
        // wake any waiter
        ::syscall(SYS_futex, &fw_.value, FUTEX_WAKE, 1, nullptr, nullptr, 0);
    }

    /** @brief 잠겨 있는지 확인합니다. */
    [[nodiscard]] bool is_locked() const noexcept {
        return fw_.load(std::memory_order_relaxed) != 0;
    }

private:
    FutexWord fw_{0};
};

// ─── FutexSemaphore ──────────────────────────────────────────────────────────

/**
 * @brief 프로세스 간 futex 카운팅 세마포어.
 *
 * SHM 내에 배치합니다. `acquire()` / `release()` 쌍으로 사용합니다.
 *
 * @code
 * // N개 슬롯 세마포어
 * auto* sem = new(shm.base()) FutexSemaphore(16);
 *
 * co_await sem->acquire(reactor); // 슬롯 1개 획득 (0이면 대기)
 * // 작업 수행...
 * sem->release(1); // 슬롯 반환
 * @endcode
 */
class FutexSemaphore {
public:
    explicit FutexSemaphore(uint32_t initial = 0) noexcept : fw_(initial) {}

    /**
     * @brief 세마포어를 획득합니다 (카운트 감소).
     *
     * 카운트가 0이면 futex wait으로 대기합니다.
     */
    Task<void> acquire(Reactor& reactor) noexcept;

    /**
     * @brief 논블로킹 tryacquire.
     *
     * @returns 성공이면 true (카운트 감소됨), 실패이면 false.
     */
    bool try_acquire() noexcept {
        uint32_t cur = fw_.load(std::memory_order_relaxed);
        while (cur > 0) {
            if (fw_.compare_exchange(cur, cur - 1, std::memory_order_acquire))
                return true;
        }
        return false;
    }

    /**
     * @brief 세마포어를 해제합니다 (카운트 증가 + waiter 깨움).
     *
     * @param count 증가할 카운트 (기본 1).
     */
    void release(uint32_t count = 1) noexcept {
        fw_.fetch_add(count, std::memory_order_release);
        ::syscall(SYS_futex, &fw_.value, FUTEX_WAKE,
                  static_cast<int>(count), nullptr, nullptr, 0);
    }

    /** @brief 현재 카운트를 반환합니다. */
    [[nodiscard]] uint32_t value() const noexcept {
        return fw_.load(std::memory_order_relaxed);
    }

private:
    FutexWord fw_;
};

} // namespace qbuem::shm

/** @} */
