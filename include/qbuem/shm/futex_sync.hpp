#pragma once

/**
 * @file qbuem/shm/futex_sync.hpp
 * @brief Futex-uring synchronization — `IORING_OP_FUTEX_WAIT/WAKE`-based IPC synchronization.
 * @defgroup qbuem_shm_futex FutexSync
 * @ingroup qbuem_shm
 *
 * ## Overview
 * Issues `IORING_OP_FUTEX_WAIT` and `IORING_OP_FUTEX_WAKE` (added in Linux 6.7+)
 * as io_uring SQEs to implement **non-blocking coroutine futex synchronization**.
 *
 * ## Advantages over futex(2)
 * | | Direct futex(2) | io_uring futex |
 * |--|-----------------|----------------|
 * | Blocking | Thread blocking | co_await, non-blocking |
 * | Batching | One at a time | SQE batch submission |
 * | Mixed I/O | Not possible | Can mix with I/O |
 *
 * ## Components
 * - `FutexWord`: Futex atomic variable in shared memory (32-bit or 64-bit).
 * - `FutexSync`: Async wait/wake helper.
 * - `FutexMutex`: Futex-based inter-process mutex (RAII).
 * - `FutexSemaphore`: Futex-based counting semaphore.
 *
 * ## Fallback
 * On kernels older than 6.7 that do not support io_uring FUTEX_WAIT,
 * falls back to `futex(2)` syscall + thread polling.
 *
 * @code
 * // Place FutexWord in shared memory
 * auto shm = SHMSegment::create("ctrl", 4096);
 * auto* fw = new (shm.base()) FutexWord(0);
 *
 * // Producer (wake)
 * fw->store(1);
 * FutexSync::wake(*fw, 1); // Wake 1 waiter
 *
 * // Consumer (wait) — inside a coroutine
 * co_await FutexSync::wait(*fw, 0, reactor); // Wait if fw == 0
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>

#include <atomic>
#include <cstdint>
#ifdef __linux__
#  include <linux/futex.h>
#  include <sys/syscall.h>
#endif
#include <unistd.h>

namespace qbuem::shm {

// ─── FutexWord ────────────────────────────────────────────────────────────────

/**
 * @brief Futex atomic variable to be placed in shared memory.
 *
 * Unlike `std::atomic<uint32_t>`, operates with the PROCESS_SHARED flag.
 * Must be placed inside a shared memory segment.
 *
 * @note Passed to the kernel without `FUTEX_PRIVATE_FLAG` to use cross-process futex.
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
 * @brief io_uring-based asynchronous futex wait/wake.
 *
 * Issues an `io_uring IORING_OP_FUTEX_WAIT` SQE so that a coroutine
 * can wait on a futex via `co_await`.
 */
class FutexSync {
public:
    /**
     * @brief Futex wait (co_await).
     *
     * Waits until wake or timeout if `fw.value == expected`.
     * Returns immediately if the value already differs.
     *
     * ## Internal Implementation
     * - With io_uring support: Issues `IORING_OP_FUTEX_WAIT` SQE.
     * - Fallback: `syscall(SYS_futex, FUTEX_WAIT)` + thread yield.
     *
     * @param fw       FutexWord in shared memory.
     * @param expected Wait termination condition (returns immediately if fw.value != expected).
     * @param reactor  Reactor to issue the io_uring SQE on.
     * @param timeout_ns Wait timeout in nanoseconds (0 = unlimited).
     */
    static Task<void> wait(FutexWord& fw, uint32_t expected,
                            Reactor& reactor, uint64_t timeout_ns = 0) noexcept;

    /**
     * @brief Futex wake — wakes up the specified number of waiters.
     *
     * Wakes via `IORING_OP_FUTEX_WAKE` or `SYS_futex FUTEX_WAKE`.
     * This function is non-blocking and can be called inside or outside a coroutine.
     *
     * @param fw      FutexWord in shared memory.
     * @param count   Maximum number of waiters to wake (INT_MAX wakes all).
     * @returns Number of waiters woken.
     */
    static int wake(FutexWord& fw, uint32_t count = 1) noexcept;

    /**
     * @brief Wakes all waiters.
     */
    static int wake_all(FutexWord& fw) noexcept {
        return wake(fw, static_cast<uint32_t>(-1));
    }

    /**
     * @brief Checks at runtime whether io_uring FUTEX is supported.
     *
     * Returns false on kernels older than 6.7.
     */
    [[nodiscard]] static bool has_uring_futex() noexcept;

private:
    // Internal function to issue io_uring SQE
    static Task<void> wait_uring(FutexWord& fw, uint32_t expected,
                                  Reactor& reactor, uint64_t timeout_ns) noexcept;

    // Fallback: syscall + thread blocking (offloaded via Reactor spawn)
    static Task<void> wait_syscall(FutexWord& fw, uint32_t expected,
                                    Reactor& reactor, uint64_t timeout_ns) noexcept;

    static int wake_syscall(FutexWord& fw, uint32_t count) noexcept;
};

// ─── FutexMutex ──────────────────────────────────────────────────────────────

/**
 * @brief Inter-process futex mutex (RAII).
 *
 * A mutex placed in SHM and shared by multiple processes.
 * Acquired by waiting via `co_await`.
 *
 * ## State
 * - `0`: Unlocked
 * - `1`: Locked (no waiters)
 * - `2`: Locked with waiters
 *
 * @code
 * FutexMutex* mtx = new(shm.base()) FutexMutex();
 *
 * // Critical section
 * auto guard = co_await mtx->lock(reactor);
 * // Perform work ...
 * // guard destructs → automatic unlock
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
     * @brief Acquires the mutex (co_await).
     *
     * If already locked, waits via futex wait.
     *
     * @param reactor For issuing io_uring SQEs.
     * @returns RAII LockGuard.
     */
    Task<LockGuard> lock(Reactor& reactor) noexcept;

    /**
     * @brief Non-blocking trylock.
     *
     * @returns LockGuard on success, nullopt on failure.
     */
    std::optional<LockGuard> try_lock() noexcept {
        uint32_t expected = 0;
        if (fw_.compare_exchange(expected, 1, std::memory_order_acquire))
            return LockGuard{this};
        return std::nullopt;
    }

    /**
     * @brief Releases the mutex.
     */
    void unlock() noexcept {
        fw_.store(0, std::memory_order_release);
#ifdef __linux__
        // wake any waiter
        ::syscall(SYS_futex, &fw_.value, FUTEX_WAKE, 1, nullptr, nullptr, 0);
#endif
    }

    /** @brief Checks whether the mutex is locked. */
    [[nodiscard]] bool is_locked() const noexcept {
        return fw_.load(std::memory_order_relaxed) != 0;
    }

private:
    FutexWord fw_{0};
};

// ─── FutexSemaphore ──────────────────────────────────────────────────────────

/**
 * @brief Inter-process futex counting semaphore.
 *
 * Placed in SHM. Used as `acquire()` / `release()` pairs.
 *
 * @code
 * // Semaphore with N slots
 * auto* sem = new(shm.base()) FutexSemaphore(16);
 *
 * co_await sem->acquire(reactor); // Acquire 1 slot (waits if count == 0)
 * // Perform work...
 * sem->release(1); // Return slot
 * @endcode
 */
class FutexSemaphore {
public:
    explicit FutexSemaphore(uint32_t initial = 0) noexcept : fw_(initial) {}

    /**
     * @brief Acquires the semaphore (decrements count).
     *
     * If count is 0, waits via futex wait.
     */
    Task<void> acquire(Reactor& reactor) noexcept;

    /**
     * @brief Non-blocking tryacquire.
     *
     * @returns true on success (count decremented), false on failure.
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
     * @brief Releases the semaphore (increments count + wakes waiters).
     *
     * @param count Amount to increment (default 1).
     */
    void release(uint32_t count = 1) noexcept {
        fw_.fetch_add(count, std::memory_order_release);
#ifdef __linux__
        ::syscall(SYS_futex, &fw_.value, FUTEX_WAKE,
                  static_cast<int>(count), nullptr, nullptr, 0);
#endif
    }

    /** @brief Returns the current count. */
    [[nodiscard]] uint32_t value() const noexcept {
        return fw_.load(std::memory_order_relaxed);
    }

private:
    FutexWord fw_;
};

} // namespace qbuem::shm

/** @} */
