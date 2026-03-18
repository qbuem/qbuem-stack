#pragma once

/**
 * @file qbuem/reactor/micro_ticker.hpp
 * @brief High-precision heartbeat engine for sub-millisecond reactor driving.
 * @defgroup qbuem_reactor_ticker MicroTicker
 * @ingroup qbuem_reactor
 *
 * ## Overview
 * `MicroTicker` is an active drive loop that achieves deterministic sub-100µs
 * tick intervals by combining:
 *   1. `nanosleep` for the coarse portion of the sleep.
 *   2. A busy-spin (PAUSE loop) for the final ~10µs to avoid re-schedule jitter.
 *   3. Drift compensation: each tick shortens/extends the next sleep to correct
 *      accumulated error, keeping the **mean frequency** perfectly stable.
 *
 * ## Passive vs. Active
 * | Mode | Mechanism | Jitter |
 * |---|---|---|
 * | Passive (`Reactor::poll(ms)`) | OS scheduler / HZ=1000 | ±500µs |
 * | Active (`MicroTicker`) | nanosleep + spin | <5µs |
 *
 * ## Reactor Integration
 * `MicroTicker` does not replace the `Reactor`; it **drives** it via `poll(0)`.
 *
 * @code
 * auto reactor = qbuem::create_reactor();
 * qbuem::MicroTicker ticker(std::chrono::microseconds{100});
 *
 * ticker.run([&](uint64_t tick_idx) {
 *     reactor->poll(0);         // non-blocking: fire ready callbacks
 *     my_logic.update(tick_idx);
 *     my_output.flush();
 * });
 * @endcode
 *
 * ## Core Affinity
 * For maximum determinism pin the calling thread before calling `run()`:
 * @code
 * qbuem::pin_thread_to_cpu(3);
 * ticker.run(cb);
 * @endcode
 */

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <functional>
#include <time.h>  // nanosleep, clock_gettime (POSIX)

#if defined(__x86_64__) || defined(__i386__)
#  include <immintrin.h>   // _mm_pause
#  define QBUEM_CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__) || defined(__arm__)
#  define QBUEM_CPU_PAUSE() __asm__ volatile("yield")
#else
#  define QBUEM_CPU_PAUSE() ((void)0)
#endif

namespace qbuem {

/**
 * @brief High-precision active heartbeat driver.
 *
 * Calls a user callback at a fixed interval with <5µs jitter by using a
 * nanosleep + busy-spin hybrid and per-tick drift compensation.
 */
class MicroTicker {
public:
    using Clock    = std::chrono::steady_clock;
    using Duration = std::chrono::nanoseconds;

    /**
     * @brief Construct a ticker with the given tick interval.
     * @param interval Desired tick period. Minimum useful value is ~5µs.
     * @param spin_threshold Duration of the final busy-spin phase.
     *                       Defaults to 10µs; reduce on fast cores.
     */
    explicit MicroTicker(
        Duration interval,
        Duration spin_threshold = std::chrono::microseconds{10}) noexcept
        : interval_(interval)
        , spin_threshold_(spin_threshold)
    {}

    /**
     * @brief Drive the tick loop synchronously until `stop()` is called.
     *
     * Blocks the calling thread. Should be called from a dedicated, CPU-pinned
     * thread. The callback receives the tick index (0, 1, 2, …).
     *
     * @param callback Invocable with signature `void(uint64_t tick_index)`.
     *                 Must complete before the next tick deadline.
     */
    template <std::invocable<uint64_t> F>
    void run(F&& callback) {
        running_.store(true, std::memory_order_relaxed);
        uint64_t tick = 0;

        auto deadline = Clock::now() + interval_;

        while (running_.load(std::memory_order_relaxed)) {
            // ── Phase 1: coarse sleep until spin_threshold_ before deadline ──
            const auto now = Clock::now();
            const auto remaining = deadline - now;
            const auto sleep_ns  = remaining - spin_threshold_;

            if (sleep_ns > Duration::zero()) {
                ::timespec ts{};
                ts.tv_sec  = static_cast<time_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(sleep_ns).count());
                ts.tv_nsec = static_cast<long>(
                    (sleep_ns - std::chrono::seconds(ts.tv_sec)).count());
                ::nanosleep(&ts, nullptr);
            }

            // ── Phase 2: busy-spin until exact deadline ───────────────────
            while (Clock::now() < deadline) {
                QBUEM_CPU_PAUSE();
            }

            // ── Phase 3: fire the callback ─────────────────────────────────
            std::forward<F>(callback)(tick++);

            // ── Phase 4: drift compensation ────────────────────────────────
            // Advance deadline by one interval. If we are already past the next
            // deadline (overrun), snap forward so the next tick isn't skipped.
            const auto next_deadline = deadline + interval_;
            const auto after_tick    = Clock::now();
            deadline = (after_tick > next_deadline) ? after_tick + interval_
                                                    : next_deadline;
        }
    }

    /**
     * @brief Request the tick loop to exit after the current callback returns.
     *
     * Thread-safe. May be called from any thread or signal handler.
     */
    void stop() noexcept {
        running_.store(false, std::memory_order_relaxed);
    }

    /** @brief Returns true while the loop is (or should be) running. */
    [[nodiscard]] bool running() const noexcept {
        return running_.load(std::memory_order_relaxed);
    }

    /** @brief Configured tick interval. */
    [[nodiscard]] Duration interval() const noexcept { return interval_; }

private:
    Duration               interval_;
    Duration               spin_threshold_;
    std::atomic<bool>      running_{false};
};

} // namespace qbuem
