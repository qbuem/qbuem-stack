#pragma once

/**
 * @file qbuem/core/tick_scheduler.hpp
 * @brief High-level multi-rate tick orchestration over the TickLoop core.
 * @defgroup qbuem_tick_scheduler Tick Scheduler
 * @ingroup qbuem_core
 *
 * `TickScheduler` is to @ref qbuem::TickLoop what a game engine's tick manager
 * is to a bare heartbeat: it runs many **systems** at independent sub-rates, in
 * a defined order, every base tick — and adds the high-level features a
 * real-time simulation needs:
 *
 * - **Multi-rate systems.** Register N callbacks, each running `every` sim ticks
 *   with a `phase` stagger and an `order` (ascending) within a tick — e.g.
 *   physics @ every tick, AI @ every 3, AOI broadcast @ every 2, metrics @ 100.
 * - **Deterministic `(seed, tick)` RNG.** A per-tick splitmix64 stream, reseeded
 *   from `(seed, tick)` each sim tick, enforces the "no live RNG" invariant that
 *   makes replay validation / cheat-proof leaderboards possible.
 * - **Pause / time-scale / single-step + interpolation alpha.** An accumulator
 *   decouples sim ticks from the real base rate: `set_time_scale()` for
 *   slow-mo/fast replay, `pause()`/`step(n)` for debugging, and `alpha()` (the
 *   sub-tick remainder in [0,1)) for client-side render interpolation.
 * - **Per-system + overall metrics.** Each system has its own work histogram
 *   (which system is the bottleneck?), all readable from a monitoring thread
 *   with zero hot-path allocation. An optional overrun handler fires when a
 *   system blows a budget (watchdog).
 *
 * Driving mirrors TickLoop: `advance(now)` (reactor coroutine / manual) or
 * `run_pinned()` (dedicated thread). System callbacks are stored in
 * `inplace_function` (no per-system heap once the slot is built; zero
 * allocation per tick).
 *
 * @code
 * qbuem::TickScheduler sched({.interval = std::chrono::milliseconds{100}}); // 10 Hz base
 * sched.set_seed(match_seed);                       // deterministic
 * sched.add_system({.every = 1, .order = 0, .name = "sim"},
 *                  [&](const qbuem::TickContext& t) { world.step(t.tick, *t.rng); });
 * sched.add_system({.every = 2, .order = 10, .name = "aoi"},
 *                  [&](const qbuem::TickContext&) { broadcast_aoi(); });
 * // reactor coroutine:
 * for (;;) { sched.advance(); co_await qbuem::sleep(sched.next_sleep_ms()); }
 * @endcode
 *
 * @{
 */

#include <qbuem/buf/inplace_function.hpp>
#include <qbuem/core/tick_loop.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace qbuem {

// ─── TickRng ──────────────────────────────────────────────────────────────────

/**
 * @brief Deterministic, seekable RNG (splitmix64) keyed by `(seed, tick)`.
 *
 * Reseeded every sim tick so the entire match is a pure function of the seed:
 * the same `(seed, tick)` always yields the same stream, on any machine — the
 * basis for replay verification and cheat-proof results. Not cryptographic.
 */
class TickRng {
public:
  /** @brief Reseed for a specific (seed, tick) — mixes both into the state. */
  void reseed(uint64_t seed, uint64_t tick) noexcept {
    state_ = seed ^ (tick * 0x9E3779B97F4A7C15ull);
  }

  /** @brief Next 64-bit value (splitmix64). */
  uint64_t next_u64() noexcept {
    uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  /** @brief Uniform integer in [0, n) (n == 0 ⇒ 0). */
  uint32_t below(uint32_t n) noexcept {
    return n == 0 ? 0u : static_cast<uint32_t>(next_u64() % n);
  }

  /** @brief Uniform double in [0, 1). */
  double unit() noexcept {
    return static_cast<double>(next_u64() >> 11) * (1.0 / 9007199254740992.0);
  }

private:
  uint64_t state_ = 0;
};

// ─── TickContext ──────────────────────────────────────────────────────────────

/**
 * @brief Delivered to each system callback for one sim tick.
 */
struct TickContext {
  /** @brief Global sim tick index — the `tick` in `f(seed, tick)`. */
  uint64_t tick = 0;
  /** @brief How many times THIS system has run (its own counter). */
  uint64_t system_runs = 0;
  /** @brief This system's timestep in seconds (`base_interval * every`). */
  double dt = 0.0;
  /** @brief Deterministic RNG for this tick (nullptr if no seed was set). */
  TickRng* rng = nullptr;
};

// ─── TickSystemConfig ─────────────────────────────────────────────────────────

/**
 * @brief Registration parameters for one system.
 */
struct TickSystemConfig {
  /** @brief Run every `every` sim ticks (rate divider; 0 treated as 1). */
  uint32_t every = 1;
  /** @brief Phase stagger: runs when `tick % every == phase % every`. */
  uint32_t phase = 0;
  /** @brief Execution order within a tick (ascending; ties keep insertion order). */
  int order = 0;
  /** @brief Human-readable name (for metrics / logs). */
  const char* name = "system";
};

// ─── TickScheduler ────────────────────────────────────────────────────────────

/**
 * @brief Multi-rate, ordered, deterministic tick orchestrator.
 *
 * Drive from a single thread. `stats()` / `system_stats()` are safe to read
 * from another thread (atomics). Control methods (pause/resume/time-scale/step/
 * set_seed) must be called from the driving thread (or before driving).
 */
class TickScheduler {
public:
  using Clock = TickLoop::Clock;
  using SystemFn = inplace_function<void(const TickContext&), 64>;

  explicit TickScheduler(TickConfig base)
      : loop_(base),
        base_interval_s_(static_cast<double>(base.interval.count()) / 1e9) {}

  // ── Registration (cold path) ────────────────────────────────────────────

  /**
   * @brief Register a system. Returns its id (for `system_stats`).
   * @param cfg Rate / phase / order / name.
   * @param fn  Invocable `void(const TickContext&)` (≤ 64 bytes captured).
   */
  template <class F>
  uint32_t add_system(TickSystemConfig cfg, F&& fn) {
    if (cfg.every == 0) cfg.every = 1;
    auto sys = std::make_unique<System>();
    sys->cfg = cfg;
    sys->fn = SystemFn(std::forward<F>(fn));
    const uint32_t id = static_cast<uint32_t>(systems_.size());
    systems_.push_back(std::move(sys));
    rebuild_order();
    return id;
  }

  // ── Control ───────────────────────────────────────────────────────────────

  /** @brief Enable deterministic per-tick RNG seeded from `seed`. */
  void set_seed(uint64_t seed) noexcept { seed_ = seed; deterministic_ = true; }
  void pause() noexcept { paused_ = true; }
  void resume() noexcept { paused_ = false; }
  [[nodiscard]] bool paused() const noexcept { return paused_; }
  /** @brief Sim ticks per base tick: 1.0 normal, 0.5 slow-mo, 2.0 fast (<0 ⇒ 0). */
  void set_time_scale(double s) noexcept { time_scale_ = s < 0.0 ? 0.0 : s; }
  [[nodiscard]] double time_scale() const noexcept { return time_scale_; }
  /** @brief Queue `n` sim ticks to run even while paused (single-stepping). */
  void step(uint32_t n = 1) noexcept { step_credits_ += n; }

  // ── Driving ─────────────────────────────────────────────────────────────

  /**
   * @brief Advance using @p now; runs all due sim ticks (and their due systems).
   * @returns The number of sim ticks run this call.
   */
  uint32_t advance(Clock::time_point now) {
    uint32_t sim = 0;
    loop_.advance(now, [&](TickInfo) { sim += pump_base_tick(); });
    return sim;
  }
  /** @overload Uses `Clock::now()`. */
  uint32_t advance() { return advance(Clock::now()); }

  /** @brief Drive synchronously on a dedicated thread until `stop()`. */
  void run_pinned() {
    loop_.run_pinned([&](TickInfo) { (void)pump_base_tick(); });
  }
  void stop() noexcept { loop_.stop(); }
  [[nodiscard]] bool running() const noexcept { return loop_.running(); }
  [[nodiscard]] int next_sleep_ms() const noexcept { return loop_.next_sleep_ms(); }
  [[nodiscard]] int next_sleep_ms(Clock::time_point now) const noexcept {
    return loop_.next_sleep_ms(now);
  }

  /** @brief Set a watchdog: `fn(system_id, work_ns)` fires when a system's work
   *  exceeds `threshold`. */
  template <class F>
  void set_overrun_handler(std::chrono::nanoseconds threshold, F&& fn) {
    overrun_threshold_ns_ = static_cast<uint64_t>(threshold.count());
    on_overrun_ = OverrunFn(std::forward<F>(fn));
  }

  // ── Introspection / metrics ─────────────────────────────────────────────

  /** @brief Current sim tick index. */
  [[nodiscard]] uint64_t tick() const noexcept { return sim_tick_; }
  /** @brief Sub-tick remainder in [0,1) toward the next sim tick (render lerp). */
  [[nodiscard]] double alpha() const noexcept {
    double a = accum_;
    return a < 0.0 ? 0.0 : (a >= 1.0 ? 0.999999 : a);
  }
  /** @brief Number of registered systems. */
  [[nodiscard]] size_t system_count() const noexcept { return systems_.size(); }
  /** @brief Overall base-loop timing stats (any thread). */
  [[nodiscard]] TickStats stats() const noexcept { return loop_.stats(); }
  /** @brief Per-system work-duration percentiles (any thread). */
  [[nodiscard]] TickStats system_stats(uint32_t id) const noexcept {
    TickStats s;
    if (id >= systems_.size()) return s;
    const System& sy = *systems_[id];
    s.ticks = sy.runs.load(std::memory_order_relaxed);
    s.work_p50_ns = sy.work.percentile(0.50);
    s.work_p99_ns = sy.work.percentile(0.99);
    s.work_p999_ns = sy.work.percentile(0.999);
    s.max_work_ns = sy.max_work_ns.load(std::memory_order_relaxed);
    return s;
  }
  /** @brief Times a system overran its budget (if a handler/threshold is set). */
  [[nodiscard]] uint64_t system_overruns(uint32_t id) const noexcept {
    return id < systems_.size()
               ? systems_[id]->overruns.load(std::memory_order_relaxed)
               : 0;
  }

  [[nodiscard]] const TickLoop& loop() const noexcept { return loop_; }

private:
  using OverrunFn = inplace_function<void(uint32_t, uint64_t), 48>;

  struct System {
    TickSystemConfig      cfg{};
    SystemFn              fn{};
    TickHistogram         work{};
    std::atomic<uint64_t> runs{0};
    std::atomic<uint64_t> max_work_ns{0};
    std::atomic<uint64_t> overruns{0};
  };

  void rebuild_order() {
    order_.resize(systems_.size());
    for (uint32_t i = 0; i < order_.size(); ++i) order_[i] = i;
    std::stable_sort(order_.begin(), order_.end(), [&](uint32_t a, uint32_t b) {
      return systems_[a]->cfg.order < systems_[b]->cfg.order;
    });
  }

  // One real base tick → accumulate sim time, run due sim ticks. Returns count.
  uint32_t pump_base_tick() {
    uint32_t fired = 0;
    if (!paused_) accum_ += time_scale_;
    while (step_credits_ > 0) {
      run_sim_tick();
      --step_credits_;
      ++fired;
    }
    while (accum_ >= 1.0) {
      run_sim_tick();
      accum_ -= 1.0;
      ++fired;
    }
    return fired;
  }

  void run_sim_tick() {
    if (deterministic_) rng_.reseed(seed_, sim_tick_);
    TickContext ctx;
    ctx.tick = sim_tick_;
    ctx.rng = deterministic_ ? &rng_ : nullptr;

    for (uint32_t idx : order_) {
      System& s = *systems_[idx];
      if ((sim_tick_ % s.cfg.every) != (s.cfg.phase % s.cfg.every)) continue;
      ctx.system_runs = s.runs.load(std::memory_order_relaxed);
      ctx.dt = base_interval_s_ * static_cast<double>(s.cfg.every);

      const auto t0 = Clock::now();
      s.fn(ctx);
      const uint64_t work = static_cast<uint64_t>((Clock::now() - t0).count());

      s.work.record(work);
      if (work > s.max_work_ns.load(std::memory_order_relaxed))
        s.max_work_ns.store(work, std::memory_order_relaxed);
      s.runs.fetch_add(1, std::memory_order_relaxed);
      if (overrun_threshold_ns_ != 0 && work > overrun_threshold_ns_) {
        s.overruns.fetch_add(1, std::memory_order_relaxed);
        if (on_overrun_) on_overrun_(idx, work);
      }
    }
    ++sim_tick_;
  }

  TickLoop                              loop_;
  double                                base_interval_s_;
  std::vector<std::unique_ptr<System>>  systems_;
  std::vector<uint32_t>                 order_;
  uint64_t                              sim_tick_ = 0;
  double                                accum_ = 0.0;
  double                                time_scale_ = 1.0;
  bool                                  paused_ = false;
  uint32_t                              step_credits_ = 0;
  uint64_t                              seed_ = 0;
  bool                                  deterministic_ = false;
  TickRng                               rng_;
  uint64_t                              overrun_threshold_ns_ = 0;
  OverrunFn                             on_overrun_{};
};

} // namespace qbuem

/** @} */
