// Tests for OffloadPool (blocking/CPU-bound offload) and Dispatcher graceful
// drain (in-flight coroutine tracking + drain()).
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/offload_pool.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace qbuem;
using namespace std::chrono_literals;

namespace {

// Drives a Dispatcher on its own thread; stops + joins on destruction.
struct Harness {
  Dispatcher   dispatcher;
  std::jthread thread;
  explicit Harness(size_t threads = 1) : dispatcher(threads) {
    thread = std::jthread([this] { dispatcher.run(); });
  }
  ~Harness() {
    dispatcher.stop();
    if (thread.joinable()) thread.join();
  }
};

// Reactor-friendly yield used by the looping test coroutines.
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

template <typename Pred>
bool wait_until(Pred p, std::chrono::milliseconds timeout = 3s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!p() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  return p();
}

} // namespace

// ── OffloadPool ───────────────────────────────────────────────────────────────

TEST(OffloadPool, RunReturnsValue) {
  Harness h;
  OffloadPool pool{2};
  std::atomic<int>  result{-1};
  std::atomic<bool> done{false};

  h.dispatcher.spawn([](OffloadPool *p, std::atomic<int> *r,
                        std::atomic<bool> *d) -> Task<void> {
    int v = co_await p->run([] { return 7 * 6; });
    r->store(v, std::memory_order_release);
    d->store(true, std::memory_order_release);
    co_return;
  }(&pool, &result, &done));

  ASSERT_TRUE(wait_until([&] { return done.load(std::memory_order_acquire); }));
  EXPECT_EQ(result.load(), 42);
  pool.shutdown();
}

TEST(OffloadPool, RunVoidCallable) {
  Harness h;
  OffloadPool pool{1};
  std::atomic<bool> ran{false};
  std::atomic<bool> done{false};

  h.dispatcher.spawn([](OffloadPool *p, std::atomic<bool> *ran,
                        std::atomic<bool> *d) -> Task<void> {
    co_await p->run([ran] { ran->store(true, std::memory_order_release); });
    d->store(true, std::memory_order_release);
    co_return;
  }(&pool, &ran, &done));

  ASSERT_TRUE(wait_until([&] { return done.load(std::memory_order_acquire); }));
  EXPECT_TRUE(ran.load());
  pool.shutdown();
}

// The blocking work runs on a POOL thread; the coroutine resumes on its
// reactor thread (not the pool thread).
TEST(OffloadPool, RunsOnPoolThreadResumesOnReactor) {
  Harness h;
  OffloadPool pool{1};
  std::atomic<bool>           done{false};
  std::atomic<std::size_t>    reactor_before{0}, reactor_after{0}, pool_tid{0};

  h.dispatcher.spawn([](OffloadPool *p, std::atomic<std::size_t> *before,
                        std::atomic<std::size_t> *after,
                        std::atomic<std::size_t> *ptid,
                        std::atomic<bool> *d) -> Task<void> {
    auto tid = [] { return std::hash<std::thread::id>{}(std::this_thread::get_id()); };
    before->store(tid(), std::memory_order_release);
    co_await p->run([ptid, tid] {
      ptid->store(tid(), std::memory_order_release);
    });
    after->store(tid(), std::memory_order_release);
    d->store(true, std::memory_order_release);
    co_return;
  }(&pool, &reactor_before, &reactor_after, &pool_tid, &done));

  ASSERT_TRUE(wait_until([&] { return done.load(std::memory_order_acquire); }));
  EXPECT_EQ(reactor_before.load(), reactor_after.load()); // resumed on origin reactor
  EXPECT_NE(pool_tid.load(), reactor_after.load());        // ran off the reactor
  pool.shutdown();
}

TEST(OffloadPool, ConcurrentOffloadsAllComplete) {
  Harness h;
  OffloadPool pool{4};
  constexpr int N = 8;
  std::atomic<int> sum{0};
  std::atomic<int> completed{0};

  for (int i = 0; i < N; ++i) {
    h.dispatcher.spawn([](OffloadPool *p, int v, std::atomic<int> *s,
                          std::atomic<int> *c) -> Task<void> {
      int r = co_await p->run([v] {
        std::this_thread::sleep_for(20ms); // simulate blocking work
        return v * v;
      });
      s->fetch_add(r, std::memory_order_relaxed);
      c->fetch_add(1, std::memory_order_acq_rel);
      co_return;
    }(&pool, i, &sum, &completed));
  }

  ASSERT_TRUE(wait_until([&] { return completed.load() == N; }));
  // 0+1+4+9+16+25+36+49 = 140
  EXPECT_EQ(sum.load(), 140);
  pool.shutdown();
}

// ── Dispatcher graceful drain ─────────────────────────────────────────────────

TEST(DispatcherDrain, InFlightTracksSpawnedCoroutines) {
  Dispatcher   d{1};
  std::jthread t([&] { d.run(); });

  std::atomic<bool> go{false};
  std::atomic<bool> finished{false};

  d.spawn([](std::atomic<bool> *go, std::atomic<bool> *fin) -> Task<void> {
    while (!go->load(std::memory_order_acquire))
      co_await Yield{};
    fin->store(true, std::memory_order_release);
    co_return;
  }(&go, &finished));

  EXPECT_TRUE(wait_until([&] { return d.in_flight() == 1; }));
  go.store(true, std::memory_order_release);
  EXPECT_TRUE(wait_until([&] { return d.in_flight() == 0; }));
  EXPECT_TRUE(finished.load());

  d.stop();
  if (t.joinable()) t.join();
}

TEST(DispatcherDrain, CleanDrainWaitsForCompletion) {
  Dispatcher   d{1};
  std::jthread t([&] { d.run(); });

  std::atomic<bool> go{false};
  std::atomic<bool> finished{false};

  d.spawn([](std::atomic<bool> *go, std::atomic<bool> *fin) -> Task<void> {
    while (!go->load(std::memory_order_acquire))
      co_await Yield{};
    fin->store(true, std::memory_order_release);
    co_return;
  }(&go, &finished));

  ASSERT_TRUE(wait_until([&] { return d.in_flight() == 1; }));
  go.store(true, std::memory_order_release);
  size_t leftover = d.drain(2s); // should wait for the coroutine, then stop
  EXPECT_EQ(leftover, 0u);
  EXPECT_TRUE(finished.load());

  if (t.joinable()) t.join();
}

// A coroutine that never completes makes drain() time out and report it.
// (Forced timeout intentionally abandons the suspended frame.)
TEST(DispatcherDrain, TimeoutReportsOutstanding) {
  Dispatcher   d{1};
  std::jthread t([&] { d.run(); });

  d.spawn([]() -> Task<void> {
    for (;;)
      co_await Yield{}; // never finishes
  }());

  ASSERT_TRUE(wait_until([&] { return d.in_flight() == 1; }));
  size_t leftover = d.drain(200ms); // times out
  EXPECT_GE(leftover, 1u);

  if (t.joinable()) t.join();
}
