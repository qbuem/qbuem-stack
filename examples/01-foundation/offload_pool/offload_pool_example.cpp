/**
 * @file examples/offload_pool_example.cpp
 * @brief OffloadPool — run blocking / CPU-bound work without stalling a reactor.
 *
 * Reactor threads must never block (Pillar 1, L1). When you must call a blocking
 * library or run a CPU-heavy transform, hand it to an `OffloadPool`:
 *
 *     result = co_await pool.run([] { return blocking_work(); });
 *
 * The work runs on a pool worker; the coroutine resumes on its ORIGIN reactor.
 *
 * This example proves the reactor is NOT blocked: a "heartbeat" coroutine keeps
 * ticking on the reactor while a "worker" coroutine offloads a 150 ms blocking
 * sleep. If the sleep had run on the reactor, the heartbeat would freeze.
 *
 * ## Build / Run
 *   cmake --build build --target offload_pool_example
 *   ./build/examples/offload_pool_example
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/offload_pool.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <qbuem/compat/print.hpp>
#include <thread>

using namespace qbuem;
using namespace std::chrono_literals;

// A reactor-friendly yield (re-post self to the reactor so other work runs).
struct Yield {
  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> h) const noexcept {
    if (auto *r = Reactor::current())
      r->post([h]() mutable { h.resume(); });
    else
      h.resume();
  }
  void await_resume() const noexcept {}
};

int main() {
  std::println("=== OffloadPool example ===");

  OffloadPool pool{2};
  std::println("[main] offload pool worker threads: {}", pool.thread_count());

  Dispatcher   dispatcher{1}; // single reactor thread — makes the point obvious
  std::jthread reactor_thread([&dispatcher] { dispatcher.run(); });

  std::atomic<uint64_t> heartbeats{0};
  std::atomic<bool>     worker_done{false};
  std::atomic<int>      offload_result{-1};

  // Heartbeat: ticks on the reactor until the worker finishes.
  dispatcher.spawn([](std::atomic<uint64_t> *hb,
                      std::atomic<bool> *done) -> Task<void> {
    while (!done->load(std::memory_order_acquire)) {
      hb->fetch_add(1, std::memory_order_relaxed);
      co_await Yield{};
    }
    co_return;
  }(&heartbeats, &worker_done));

  // Worker: offloads a 150 ms BLOCKING sleep to the pool; the reactor stays free.
  dispatcher.spawn([](OffloadPool *p, std::atomic<int> *out,
                      std::atomic<bool> *done) -> Task<void> {
    std::println("[worker] reactor thread = {}",
                 std::hash<std::thread::id>{}(std::this_thread::get_id()));

    int value = co_await p->run([] {
      std::println("[offload] running on pool thread = {} (blocking 150ms)",
                   std::hash<std::thread::id>{}(std::this_thread::get_id()));
      std::this_thread::sleep_for(150ms); // a blocking call — safe off-reactor
      return 21 * 2;
    });

    std::println("[worker] resumed on reactor thread = {}; result = {}",
                 std::hash<std::thread::id>{}(std::this_thread::get_id()), value);
    out->store(value, std::memory_order_release);
    done->store(true, std::memory_order_release);
    co_return;
  }(&pool, &offload_result, &worker_done));

  // Wait for the worker (from the main thread).
  while (!worker_done.load(std::memory_order_acquire))
    std::this_thread::sleep_for(2ms);

  std::println("\n[main] result          = {}", offload_result.load());
  std::println("[main] heartbeats while offloaded = {}", heartbeats.load());
  std::println("[main] => reactor stayed responsive during the blocking call: {}",
               heartbeats.load() > 1 ? "YES" : "NO");

  // Graceful shutdown: drain in-flight coroutines, then stop; then the pool.
  std::println("\n[main] draining dispatcher (in_flight={})...", dispatcher.in_flight());
  size_t leftover = dispatcher.drain(2s);
  std::println("[main] drain complete, coroutines still in flight = {}", leftover);
  if (reactor_thread.joinable()) reactor_thread.join();
  pool.shutdown();

  std::println("[main] done.");
  return 0;
}
