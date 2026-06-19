/**
 * @file examples/01-foundation/tick_loop/tick_loop_example.cpp
 * @brief TickLoop — precise, drift-free, deterministic fixed-timestep ticking.
 *
 * Demonstrates the two ways to drive a TickLoop:
 *   (1) run_pinned()  — a dedicated thread with nanosleep + busy-spin for
 *       sub-millisecond control/audio/HFT loops (here: 1 kHz for 300 ms).
 *   (2) advance(now)  — manual / reactor-coroutine drive with a synthetic clock,
 *       showing drift-free deadlines and deterministic catch-up after a stall.
 *
 * Build:  cmake --build build --target tick_loop_example
 * Run:    ./build/examples/tick_loop_example
 */

#include <qbuem/compat/print.hpp>
#include <qbuem/core/tick_loop.hpp>

#include <chrono>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;
using std::println;

static void print_stats(const char* label, const TickStats& s) {
  println("[{}] ticks={} overruns={} catchups={} backlog_events={}", label,
          s.ticks, s.overruns, s.catchups, s.backlog_events);
  println("    jitter  p50={}us p99={}us p99.9={}us  max_lateness={}us",
          s.jitter_p50_ns / 1000, s.jitter_p99_ns / 1000,
          s.jitter_p999_ns / 1000, s.max_lateness_ns / 1000);
  println("    work    p50={}us p99={}us p99.9={}us  max_work={}us  load={:.3f}",
          s.work_p50_ns / 1000, s.work_p99_ns / 1000, s.work_p999_ns / 1000,
          s.max_work_ns / 1000, s.load);
}

int main() {
  // ── (1) Pinned high-rate loop: 1 kHz for ~300 ms ──────────────────────────
  println("== (1) run_pinned: 1 kHz precise heartbeat for 300 ms ==");
  TickLoop hi({.interval = 1ms, .spin_window = 100us});
  std::atomic<uint64_t> work_accum{0};

  std::jthread driver([&] {
    // qbuem::pin_thread_to_cpu(2);  // pin for best determinism in production
    hi.run_pinned([&](TickInfo t) {
      // Trivial deterministic work — a real loop would step a simulation here.
      work_accum.fetch_add(t.index, std::memory_order_relaxed);
    });
  });
  std::this_thread::sleep_for(300ms);
  hi.stop();
  driver.join();
  print_stats("1kHz", hi.stats());
  println("    (≈300 ticks expected; jitter p99 shows scheduler precision)\n");

  // ── (2) Manual drive with a synthetic clock: drift-free + catch-up ────────
  println("== (2) advance(): drift-free deadlines + deterministic catch-up ==");
  TickLoop sim({.interval = 100ms, .max_catchup = 8}); // 10 Hz game tick
  const auto t0 = TickLoop::Clock::now();
  std::vector<uint64_t> order;
  auto step = [&](TickInfo t) { order.push_back(t.index); };

  sim.advance(t0, step);              // establish the first deadline
  sim.advance(t0 + 100ms, step);     // tick 0, on time
  sim.advance(t0 + 205ms, step);     // tick 1, 5 ms late — deadline stays at 200
  // A 320 ms stall: ticks for deadlines 300, 400, 500 ALL execute in order
  // (deterministic — no skipped (seed,tick)).
  uint32_t fired = sim.advance(t0 + 520ms, step);
  println("    after a 320 ms stall, advance() fired {} ticks (catch-up)", fired);
  std::string seq;
  for (auto i : order) { seq += std::to_string(i); seq += ' '; }
  println("    tick order (contiguous, none skipped): {}", seq);
  print_stats("10Hz", sim.stats());

  println("\nWhere this is useful: game-server ticks, physics steps, robotics/PID "
          "control, audio block processing, market-data strobes, fixed-rate "
          "sensor sampling — anywhere a precise, drift-free, instrumented "
          "heartbeat matters.");
  return 0;
}
