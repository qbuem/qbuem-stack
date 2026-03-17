#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Helper: run a Dispatcher for up to `duration`, then stop it.
// ---------------------------------------------------------------------------
static void run_for(qbuem::Dispatcher &d, std::chrono::milliseconds duration) {
  std::jthread stopper([&] {
    std::this_thread::sleep_for(duration);
    d.stop();
  });
  d.run();
  // std::jthread auto-joins on destruction.
}

// ---------------------------------------------------------------------------
// TEST: post() executes a callback on a worker thread.
// ---------------------------------------------------------------------------
TEST(DispatcherPost, PostRunsOnWorker) {
  qbuem::Dispatcher d(1);

  std::atomic<bool> called{false};
  std::atomic<std::thread::id> worker_tid{};

  // Give the reactor thread time to start, then post.
  std::jthread poster([&] {
    std::this_thread::sleep_for(10ms);
    d.post([&] {
      called = true;
      worker_tid = std::this_thread::get_id();
    });
    // Wait for callback, then stop.
    for (int i = 0; i < 100 && !called; ++i)
      std::this_thread::sleep_for(5ms);
    d.stop();
  });

  d.run();
  poster.join();

  EXPECT_TRUE(called.load());
  // The callback must have run on the worker, not the poster thread.
  EXPECT_NE(worker_tid.load(), poster.get_id());
}

// ---------------------------------------------------------------------------
// TEST: post_to() targets a specific reactor.
// ---------------------------------------------------------------------------
TEST(DispatcherPost, PostToSpecificReactor) {
  constexpr size_t N = 3;
  qbuem::Dispatcher d(N);

  std::atomic<int> count{0};

  std::jthread poster([&] {
    std::this_thread::sleep_for(10ms);
    for (size_t i = 0; i < N; ++i) {
      d.post_to(i, [&] { count.fetch_add(1); });
    }
    for (int i = 0; i < 200 && count.load() < (int)N; ++i)
      std::this_thread::sleep_for(5ms);
    d.stop();
  });

  d.run();
  poster.join();

  EXPECT_EQ(count.load(), (int)N);
}

// ---------------------------------------------------------------------------
// TEST: spawn() runs a Task<void> coroutine fire-and-forget.
// ---------------------------------------------------------------------------
qbuem::Task<void> simple_coro(std::atomic<int> &counter) {
  counter.fetch_add(1);
  co_return;
}

TEST(DispatcherPost, SpawnFireAndForget) {
  qbuem::Dispatcher d(1);

  std::atomic<int> counter{0};

  std::jthread poster([&] {
    std::this_thread::sleep_for(10ms);
    d.spawn(simple_coro(counter));
    for (int i = 0; i < 100 && counter.load() < 1; ++i)
      std::this_thread::sleep_for(5ms);
    d.stop();
  });

  d.run();
  poster.join();

  EXPECT_EQ(counter.load(), 1);
}

// ---------------------------------------------------------------------------
// TEST: spawn_on() runs a coroutine on a specific reactor.
// ---------------------------------------------------------------------------
TEST(DispatcherPost, SpawnOnSpecificReactor) {
  constexpr size_t N = 2;
  qbuem::Dispatcher d(N);

  std::atomic<int> counter{0};

  std::jthread poster([&] {
    std::this_thread::sleep_for(10ms);
    d.spawn_on(0, simple_coro(counter));
    d.spawn_on(1, simple_coro(counter));
    for (int i = 0; i < 100 && counter.load() < 2; ++i)
      std::this_thread::sleep_for(5ms);
    d.stop();
  });

  d.run();
  poster.join();

  EXPECT_EQ(counter.load(), 2);
}
