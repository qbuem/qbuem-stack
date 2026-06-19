#pragma once

/**
 * @file qbuem/core/tick_loop.hpp
 * @brief Drift-compensated, deterministic fixed-timestep tick loop with
 *        zero-allocation, cross-thread-readable latency metrics.
 * @defgroup qbuem_tick_loop Tick Loop
 * @ingroup qbuem_core
 *
 * `TickLoop` is the precise heartbeat for simulations and real-time control:
 * game-server ticks, physics steps, control loops (robotics/PID), audio block
 * processing, market-data strobes, fixed-rate sensor sampling. It solves the
 * three problems a naive `sleep(interval)` loop has:
 *
 * 1. **Drift.** A naive loop schedules `now + interval`, so each tick's own work
 *    time accumulates as error and the mean rate sags. `TickLoop` advances an
 *    absolute deadline by exactly `interval` every tick, so the long-run
 *    frequency is exact (no drift).
 * 2. **Determinism / catch-up.** If a wakeup is late (the OS slept too long, or
 *    a tick overran), every *missed* tick is still executed in order — so a
 *    deterministic `state = f(seed, tick)` simulation never skips a tick. Per
 *    wakeup, catch-up is bounded by `max_catchup` to prevent a spiral of death;
 *    remaining ticks run on the next wakeup (`behind()` reports the backlog).
 * 3. **Observability.** Per-tick *jitter* (scheduled-vs-actual fire time) and
 *    *work* duration are recorded into fixed-bucket histograms with atomic
 *    counters — so `stats()` can be read from another thread (e.g. an HTTP
 *    metrics endpoint) while the tick thread runs, with **zero allocation** on
 *    the hot path.
 *
 * ## Two ways to drive it
 *
 * **(A) Reactor coroutine** (shares one reactor thread with I/O — the
 * single-reactor game-server model; no busy-spin, no extra thread):
 * @code
 * qbuem::TickLoop loop({.interval = std::chrono::milliseconds{100}}); // 10 Hz
 * // inside a Task<void> spawned on the dispatcher:
 * for (;;) {
 *   loop.advance([&](qbuem::TickInfo t) { world.step(t.index); broadcast(); });
 *   co_await qbuem::sleep(loop.next_sleep_ms());   // drift-compensated wait
 * }
 * @endcode
 *
 * **(B) Dedicated pinned thread** (ultra-low-latency sub-millisecond loops —
 * control/audio/HFT; nanosleep + busy-spin, like @ref qbuem::MicroTicker but
 * with catch-up + metrics):
 * @code
 * qbuem::pin_thread_to_cpu(3);
 * qbuem::TickLoop loop({.interval = std::chrono::microseconds{250}});  // 4 kHz
 * loop.run_pinned([&](qbuem::TickInfo t) { control.update(t.index); });
 * @endcode
 *
 * @{
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <time.h>  // nanosleep (POSIX)

#if defined(__x86_64__) || defined(__i386__)
#  include <immintrin.h>
#  define QBUEM_TICK_PAUSE() _mm_pause()
#elif defined(__aarch64__) || defined(__arm__)
#  define QBUEM_TICK_PAUSE() __asm__ volatile("yield")
#else
#  define QBUEM_TICK_PAUSE() ((void)0)
#endif

namespace qbuem {

// ─── TickInfo ─────────────────────────────────────────────────────────────────

/**
 * @brief Context delivered to the tick callback for one tick.
 */
struct TickInfo {
  /** @brief Monotonic tick index (0, 1, 2, …) — the `tick` in `f(seed, tick)`. */
  uint64_t index = 0;
  /** @brief Nanoseconds the fire was late vs its scheduled deadline (≥ 0). */
  uint64_t lateness_ns = 0;
  /** @brief True if this tick is a catch-up tick (run back-to-back after a late
   *  wakeup), false for an on-schedule tick. */
  bool is_catchup = false;
};

// ─── TickHistogram ────────────────────────────────────────────────────────────

/**
 * @brief Fixed-bucket latency histogram (zero-alloc, atomic, cross-thread read).
 *
 * Buckets are explicit nanosecond upper bounds tuned for tick jitter/work
 * (100 ns … 1 s). `record()` runs on the tick thread; `percentile()`/`count()`
 * may be read concurrently from another thread (relaxed atomics — exact for
 * monitoring).
 */
class TickHistogram {
public:
  /** @brief Bucket upper bounds in nanoseconds (1 µs … 1 s; last is catch-all). */
  static constexpr std::array<uint64_t, 16> kBounds = {
      1'000,        5'000,        10'000,        25'000,
      50'000,       100'000,      250'000,       500'000,
      1'000'000,    5'000'000,    10'000'000,    50'000'000,
      100'000'000,  500'000'000,  1'000'000'000, 0xFFFFFFFFFFFFFFFFull};

  /** @brief Record a sample (nanoseconds). Hot-path; ~16 comparisons, no alloc. */
  void record(uint64_t ns) noexcept {
    for (size_t i = 0; i < kBounds.size(); ++i) {
      if (ns <= kBounds[i]) {
        counts_[i].fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
  }

  /** @brief Total recorded samples. */
  [[nodiscard]] uint64_t count() const noexcept {
    uint64_t t = 0;
    for (const auto& c : counts_) t += c.load(std::memory_order_relaxed);
    return t;
  }

  /**
   * @brief Approximate percentile (returns the bucket upper bound it falls in).
   * @param p Fraction in [0, 1], e.g. 0.99 for p99.
   * @returns The ns upper bound of the bucket containing the p-th sample
   *          (resolution is the bucket width); 0 if no samples.
   */
  [[nodiscard]] uint64_t percentile(double p) const noexcept {
    uint64_t total = count();
    if (total == 0) return 0;
    // ceil so e.g. p99.9 of 100 samples needs the 100th sample (the outlier),
    // not the 99th — otherwise tail outliers are invisible.
    uint64_t threshold = static_cast<uint64_t>(std::ceil(p * static_cast<double>(total)));
    if (threshold == 0) threshold = 1;
    uint64_t cum = 0;
    for (size_t i = 0; i < kBounds.size(); ++i) {
      cum += counts_[i].load(std::memory_order_relaxed);
      if (cum >= threshold) return kBounds[i];
    }
    return kBounds.back();
  }

  void reset() noexcept {
    for (auto& c : counts_) c.store(0, std::memory_order_relaxed);
  }

private:
  std::array<std::atomic<uint64_t>, 16> counts_{};
};

// ─── TickStats ────────────────────────────────────────────────────────────────

/**
 * @brief Snapshot of tick-loop health (cheap to take; safe from any thread).
 */
struct TickStats {
  uint64_t ticks = 0;            ///< Total ticks executed.
  uint64_t overruns = 0;         ///< Ticks whose work exceeded one interval.
  uint64_t catchups = 0;         ///< Catch-up ticks run after late wakeups.
  uint64_t backlog_events = 0;   ///< Wakeups that hit the max_catchup cap (falling behind).
  uint64_t max_lateness_ns = 0;  ///< Worst scheduled-vs-fire lateness seen.
  uint64_t max_work_ns = 0;      ///< Worst callback duration seen.
  uint64_t jitter_p50_ns = 0, jitter_p99_ns = 0, jitter_p999_ns = 0;
  uint64_t work_p50_ns = 0, work_p99_ns = 0, work_p999_ns = 0;
  double   load = 0.0;           ///< EWMA(work / interval); ≥ 1.0 ⇒ cannot keep up.
  uint64_t behind = 0;           ///< Ticks currently due but not yet run.
};

// ─── TickConfig ───────────────────────────────────────────────────────────────

/**
 * @brief Tunable parameters for a `TickLoop`.
 */
struct TickConfig {
  /** @brief Tick period (e.g. 100 ms for 10 Hz, 250 µs for 4 kHz). */
  std::chrono::nanoseconds interval{std::chrono::milliseconds{100}};

  /** @brief Max catch-up ticks fired per `advance()`/wakeup beyond the first.
   *  Bounds per-wakeup work so a late wakeup cannot trigger a spiral of death;
   *  any further due ticks run on the next wakeup. */
  uint32_t max_catchup = 8;

  /** @brief (run_pinned only) Busy-spin window before the deadline, ns. The
   *  coarse wait is `nanosleep`; the final `spin_window` is a PAUSE spin to
   *  remove scheduler jitter. Ignored in the reactor/`advance()` path. */
  std::chrono::nanoseconds spin_window{std::chrono::microseconds{50}};
};

// ─── TickLoop ─────────────────────────────────────────────────────────────────

/**
 * @brief Drift-compensated fixed-timestep tick loop with 0-alloc metrics.
 *
 * Not internally synchronized for *driving* (drive from a single thread). The
 * metrics (`stats()`/histograms) ARE safe to read concurrently from another
 * thread.
 */
class TickLoop {
public:
  using Clock = std::chrono::steady_clock;

  explicit TickLoop(TickConfig cfg) noexcept
      : cfg_(cfg),
        interval_(cfg.interval <= std::chrono::nanoseconds::zero()
                      ? std::chrono::nanoseconds{1}
                      : cfg.interval) {}

  /**
   * @brief Fire all ticks due as of @p now, bounded by `max_catchup`.
   *
   * Drives the loop from an externally-supplied clock reading — usable from a
   * reactor coroutine, a manual loop, or a test with a synthetic clock. Every
   * due tick (including missed ones) is executed in order; the first deadline is
   * established on the first call.
   *
   * @param now Current time (typically `Clock::now()`).
   * @param fn  Invocable `void(TickInfo)`. Must not outlive this call.
   * @returns Number of ticks fired this call.
   */
  template <std::invocable<TickInfo> F>
  uint32_t advance(Clock::time_point now, F&& fn) {
    if (!started_) {
      next_deadline_ = now + interval_;
      started_ = true;
      return 0;
    }

    uint32_t fired = 0;
    const uint32_t budget = cfg_.max_catchup + 1;
    while (now >= next_deadline_ && fired < budget) {
      const uint64_t lateness =
          static_cast<uint64_t>((now - next_deadline_).count());
      TickInfo info{tick_, lateness, fired > 0};

      hist_jitter_.record(lateness);
      if (lateness > max_lateness_ns_.load(std::memory_order_relaxed))
        max_lateness_ns_.store(lateness, std::memory_order_relaxed);

      const auto t0 = Clock::now();
      fn(info);
      const uint64_t work = static_cast<uint64_t>((Clock::now() - t0).count());

      hist_work_.record(work);
      if (work > max_work_ns_.load(std::memory_order_relaxed))
        max_work_ns_.store(work, std::memory_order_relaxed);
      if (work > static_cast<uint64_t>(interval_.count()))
        overruns_.fetch_add(1, std::memory_order_relaxed);
      // EWMA load (1/16 weight) — fixed-point-free, monitoring only.
      const double inst = static_cast<double>(work) /
                          static_cast<double>(interval_.count());
      load_ = load_ + (inst - load_) * 0.0625;

      ++tick_;
      ++fired;
      if (fired > 1) catchups_.fetch_add(1, std::memory_order_relaxed);
      next_deadline_ += interval_;
      // `now` is fixed for this call: all ticks due as of the supplied clock
      // reading fire back-to-back (bounded by the budget). Any further ticks
      // that come due while this batch runs are picked up on the next call,
      // whose fresh clock reading reflects the elapsed work time.
    }

    ticks_.store(tick_, std::memory_order_relaxed);
    // Still behind after spending the budget → defer the rest to the next call.
    if (now >= next_deadline_)
      backlog_events_.fetch_add(1, std::memory_order_relaxed);
    return fired;
  }

  /** @overload Uses `Clock::now()`. */
  template <std::invocable<TickInfo> F>
  uint32_t advance(F&& fn) {
    return advance(Clock::now(), std::forward<F>(fn));
  }

  /**
   * @brief Milliseconds until the next deadline, for a reactor `sleep()` wait.
   * @returns 0 if a tick is already due (wake immediately to catch up), else the
   *          rounded-up ms to the next deadline (minimum 1).
   */
  [[nodiscard]] int next_sleep_ms(Clock::time_point now) const noexcept {
    if (!started_) return rounded_ms(interval_);
    const auto rem = next_deadline_ - now;
    if (rem <= std::chrono::nanoseconds::zero()) return 0;
    return rounded_ms(rem);
  }
  /** @overload Uses `Clock::now()`. */
  [[nodiscard]] int next_sleep_ms() const noexcept {
    return next_sleep_ms(Clock::now());
  }

  /** @brief Ticks currently due but not yet run (backlog) as of @p now. */
  [[nodiscard]] uint64_t behind(Clock::time_point now) const noexcept {
    if (!started_ || now < next_deadline_) return 0;
    return static_cast<uint64_t>((now - next_deadline_).count()) /
               static_cast<uint64_t>(interval_.count()) +
           1;
  }
  /** @overload Uses `Clock::now()`. */
  [[nodiscard]] uint64_t behind() const noexcept { return behind(Clock::now()); }

  /**
   * @brief Drive synchronously on a dedicated (ideally CPU-pinned) thread until
   *        `stop()`, using nanosleep + busy-spin for sub-millisecond precision.
   *
   * @param fn Invocable `void(TickInfo)`.
   */
  template <std::invocable<TickInfo> F>
  void run_pinned(F&& fn) {
    running_.store(true, std::memory_order_relaxed);
    next_deadline_ = Clock::now() + interval_;
    started_ = true;

    while (running_.load(std::memory_order_relaxed)) {
      precise_wait_until(next_deadline_);
      if (!running_.load(std::memory_order_relaxed)) break;
      advance(Clock::now(), fn);
    }
  }

  /** @brief Ask `run_pinned()` to exit. Thread-safe / signal-safe. */
  void stop() noexcept { running_.store(false, std::memory_order_relaxed); }
  [[nodiscard]] bool running() const noexcept {
    return running_.load(std::memory_order_relaxed);
  }

  /** @brief Take a metrics snapshot (safe from any thread). */
  [[nodiscard]] TickStats stats() const noexcept {
    TickStats s;
    s.ticks = ticks_.load(std::memory_order_relaxed);
    s.overruns = overruns_.load(std::memory_order_relaxed);
    s.catchups = catchups_.load(std::memory_order_relaxed);
    s.backlog_events = backlog_events_.load(std::memory_order_relaxed);
    s.max_lateness_ns = max_lateness_ns_.load(std::memory_order_relaxed);
    s.max_work_ns = max_work_ns_.load(std::memory_order_relaxed);
    s.jitter_p50_ns = hist_jitter_.percentile(0.50);
    s.jitter_p99_ns = hist_jitter_.percentile(0.99);
    s.jitter_p999_ns = hist_jitter_.percentile(0.999);
    s.work_p50_ns = hist_work_.percentile(0.50);
    s.work_p99_ns = hist_work_.percentile(0.99);
    s.work_p999_ns = hist_work_.percentile(0.999);
    s.load = load_;
    s.behind = behind();
    return s;
  }

  [[nodiscard]] uint64_t tick_count() const noexcept {
    return ticks_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::chrono::nanoseconds interval() const noexcept {
    return interval_;
  }
  [[nodiscard]] const TickHistogram& jitter_histogram() const noexcept {
    return hist_jitter_;
  }
  [[nodiscard]] const TickHistogram& work_histogram() const noexcept {
    return hist_work_;
  }

private:
  static int rounded_ms(std::chrono::nanoseconds ns) noexcept {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  ns + std::chrono::nanoseconds{999'999})
                  .count();
    return ms < 1 ? 1 : static_cast<int>(ms);
  }

  void precise_wait_until(Clock::time_point deadline) noexcept {
    const auto now = Clock::now();
    const auto sleep_ns = (deadline - now) - cfg_.spin_window;
    if (sleep_ns > std::chrono::nanoseconds::zero()) {
      ::timespec ts{};
      ts.tv_sec = static_cast<time_t>(
          std::chrono::duration_cast<std::chrono::seconds>(sleep_ns).count());
      ts.tv_nsec = static_cast<long>(
          (sleep_ns - std::chrono::seconds(ts.tv_sec)).count());
      ::nanosleep(&ts, nullptr);
    }
    while (running_.load(std::memory_order_relaxed) && Clock::now() < deadline)
      QBUEM_TICK_PAUSE();
  }

  TickConfig               cfg_;
  std::chrono::nanoseconds interval_;
  Clock::time_point        next_deadline_{};
  uint64_t                 tick_ = 0;
  bool                     started_ = false;
  std::atomic<bool>        running_{false};

  // Metrics (atomic → readable from a monitoring thread; relaxed = monitoring).
  std::atomic<uint64_t> ticks_{0};
  std::atomic<uint64_t> overruns_{0};
  std::atomic<uint64_t> catchups_{0};
  std::atomic<uint64_t> backlog_events_{0};
  std::atomic<uint64_t> max_lateness_ns_{0};
  std::atomic<uint64_t> max_work_ns_{0};
  double                load_ = 0.0;
  TickHistogram         hist_jitter_;
  TickHistogram         hist_work_;
};

} // namespace qbuem

/** @} */
