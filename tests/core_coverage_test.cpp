/**
 * @file tests/core_coverage_test.cpp
 * @brief Coverage tests for lightly-tested core primitives.
 *
 * Modules exercised here (none duplicated from existing test files):
 *   - core/task.hpp          : Task<T> value path, Task<void>, detach,
 *                              nested co_await, error propagation via Result.
 *   - core/awaiters.hpp      : AsyncSleep ready/value, sleep() factory,
 *                              reactor-less fallback (resume immediately).
 *   - core/cpu_hints.hpp     : prefetch_read/write/ahead, cpu_pause,
 *                              compiler_barrier, kCacheLineSize, CacheLinePad,
 *                              LIKELY/UNLIKELY macros.
 *   - core/session_store.hpp : ISessionStore interface via a tiny in-process
 *                              mock (get/set/del/touch/exists + error path).
 *   - core/transport.hpp +
 *     transport/plain_transport.hpp : PlainTransport ctor/fd/handshake no-op,
 *                              empty read/write, close idempotency,
 *                              negotiated_protocol default.
 *   - core/micro_ticker.hpp  : run a bounded number of ticks then stop().
 *   - core/huge_pages.hpp    : HugeBufferPool acquire/release/available/
 *                              capacity (graceful mmap fallback).
 *   - core/numa.hpp          : numa_node_count, cpu_to_numa_map,
 *                              pin_reactor_to_cpu / auto_numa_bind graceful
 *                              fallback, PerfCounters availability.
 *
 * All tests are deterministic, single-process, no real network sockets and no
 * wall-clock-dependent correctness.
 */

#include <qbuem/core/task.hpp>
#include <qbuem/core/awaiters.hpp>
#include <qbuem/core/cpu_hints.hpp>
#include <qbuem/core/session_store.hpp>
#include <qbuem/core/transport.hpp>
#include <qbuem/transport/plain_transport.hpp>
#include <qbuem/core/micro_ticker.hpp>
#include <qbuem/core/huge_pages.hpp>
#include <qbuem/core/numa.hpp>
#include <qbuem/core/dispatcher.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace qbuem;

// ───────────────────────────────────────────────────────────────────────────
// Synchronous coroutine driver.
//
// Task<T> uses suspend_always at initial_suspend. For coroutines that only
// co_return immediately or co_await already-resolvable inner Tasks (symmetric
// transfer runs them synchronously), a single handle.resume() runs the whole
// coroutine to completion. We then read the promise value directly.
// ───────────────────────────────────────────────────────────────────────────

template <typename T>
static T sync_value(Task<T>&& t) {
  t.handle.resume();
  return std::move(*t.handle.promise().value);
}

static void sync_void(Task<void>&& t) {
  t.handle.resume();
}

// ===========================================================================
// task.hpp
// ===========================================================================

namespace {
Task<int> make_int(int v) { co_return v; }

Task<int> add_via_await(int a, int b) {
  int x = co_await make_int(a);
  int y = co_await make_int(b);
  co_return x + y;
}

Task<Result<int>> ok_result(int v) { co_return Result<int>{v}; }

Task<Result<int>> err_result() {
  co_return unexpected(std::make_error_code(std::errc::invalid_argument));
}

Task<Result<int>> propagate_error() {
  auto r = co_await err_result();
  if (!r) co_return unexpected(r.error());
  co_return r.value() + 1;
}

Task<void> set_flag(std::shared_ptr<std::atomic<int>> counter) {
  counter->fetch_add(1, std::memory_order_relaxed);
  co_return;
}
}  // namespace

TEST(CoreTask, ValuePath) {
  EXPECT_EQ(sync_value(make_int(42)), 42);
}

TEST(CoreTask, NestedCoAwait) {
  EXPECT_EQ(sync_value(add_via_await(3, 4)), 7);
}

TEST(CoreTask, ResultOkPath) {
  auto r = sync_value(ok_result(10));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r.value(), 10);
}

TEST(CoreTask, ResultErrorPath) {
  auto r = sync_value(err_result());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(CoreTask, ErrorPropagatesThroughAwait) {
  auto r = sync_value(propagate_error());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(CoreTask, VoidTaskRuns) {
  auto counter = std::make_shared<std::atomic<int>>(0);
  sync_void(set_flag(counter));
  EXPECT_EQ(counter->load(), 1);
}

TEST(CoreTask, AwaitReadyReflectsDone) {
  // Fresh task (initial_suspend == suspend_always) is not yet done.
  Task<int> t = make_int(5);
  EXPECT_FALSE(t.await_ready());
  t.handle.resume();  // run to completion
  EXPECT_TRUE(t.await_ready());
  EXPECT_EQ(std::move(*t.handle.promise().value), 5);
}

TEST(CoreTask, MoveConstructionTransfersOwnership) {
  Task<int> a = make_int(99);
  auto* h = a.handle.address();
  Task<int> b = std::move(a);
  EXPECT_EQ(a.handle.address(), nullptr);
  EXPECT_EQ(b.handle.address(), h);
  EXPECT_EQ(sync_value(std::move(b)), 99);
}

TEST(CoreTask, MoveAssignmentReplacesHandle) {
  Task<int> a = make_int(1);
  Task<int> b = make_int(2);
  b = std::move(a);  // destroys b's frame, takes a's
  EXPECT_EQ(a.handle.address(), nullptr);
  EXPECT_EQ(sync_value(std::move(b)), 1);
}

TEST(CoreTask, DetachSelfManagesFrame) {
  // After detach() the Task no longer owns the frame; resuming the detached
  // handle to completion lets the frame destroy itself (no leak / no double
  // free under ASan).
  Task<void> t = set_flag(std::make_shared<std::atomic<int>>(0));
  auto handle = t.handle;
  t.detach();
  EXPECT_EQ(t.handle.address(), nullptr);  // ownership released
  // Drive the self-managing frame to completion; it destroys itself.
  handle.resume();
  // No further access to handle after this point (frame is gone).
  SUCCEED();
}

TEST(CoreTask, ResumeOnCompletedReturnsFalse) {
  Task<int> t = make_int(8);
  // First resume runs to completion (returns false since handle.done()).
  EXPECT_FALSE(t.resume());
  // Second resume on a done coroutine also returns false.
  EXPECT_FALSE(t.resume());
  EXPECT_EQ(std::move(*t.handle.promise().value), 8);
}

// ===========================================================================
// awaiters.hpp
// ===========================================================================

namespace {
// AsyncSleep with timeout<=0 is await_ready==true → resumes without a reactor.
Task<int> sleep_then_value() {
  co_await AsyncSleep{0};        // ready immediately
  co_await qbuem::sleep(-5);     // factory, also ready immediately
  co_return 7;
}
}  // namespace

TEST(CoreAwaiters, AsyncSleepReadyForNonPositiveTimeout) {
  EXPECT_TRUE((AsyncSleep{0}.await_ready()));
  EXPECT_TRUE((AsyncSleep{-1}.await_ready()));
  EXPECT_FALSE((AsyncSleep{10}.await_ready()));
}

TEST(CoreAwaiters, SleepFactoryProducesAwaiter) {
  AsyncSleep s = qbuem::sleep(25);
  EXPECT_EQ(s.timeout_ms, 25);
}

TEST(CoreAwaiters, AsyncSleepReadyPathRunsInline) {
  // No reactor installed on this thread; ready (timeout<=0) awaiters never
  // touch the reactor, so the coroutine completes synchronously.
  EXPECT_EQ(sync_value(sleep_then_value()), 7);
}

TEST(CoreAwaiters, ReadWriteAwaiterAreNotReady) {
  AsyncRead r{.fd = -1, .buf = nullptr, .count = 0};
  AsyncWrite w{.fd = -1, .buf = nullptr, .count = 0};
  AsyncAccept a{.listen_fd = -1};
  EXPECT_FALSE(r.await_ready());
  EXPECT_FALSE(w.await_ready());
  EXPECT_FALSE(a.await_ready());
  // Default-result accessors before any I/O.
  EXPECT_EQ(r.await_resume(), -1);
  EXPECT_EQ(w.await_resume(), -1);
  EXPECT_EQ(a.await_resume(), -1);
}

// ===========================================================================
// cpu_hints.hpp
// ===========================================================================

TEST(CoreCpuHints, CacheLineSizeIsPowerOfTwoAndReasonable) {
  EXPECT_GE(kCacheLineSize, 16u);
  EXPECT_LE(kCacheLineSize, 256u);
  // power of two
  EXPECT_EQ(kCacheLineSize & (kCacheLineSize - 1), 0u);
}

TEST(CoreCpuHints, PrefetchAndPauseAreCallable) {
  int data[16] = {};
  // Various locality template instantiations compile and run.
  prefetch_read(&data[0]);
  prefetch_read<0>(&data[1]);
  prefetch_read<2>(&data[2]);
  prefetch_write(&data[3]);
  prefetch_write<1>(&data[4]);
  cpu_pause();
  compiler_barrier();
  SUCCEED();
}

TEST(CoreCpuHints, PrefetchAheadBoundsCheck) {
  std::vector<int> arr(10, 0);
  // cur near the end: next index out of bounds → no-op, must not crash.
  prefetch_ahead(arr.data(), /*cur=*/9, /*count=*/arr.size());
  // Ahead is the SECOND template param (T is first, deduced); use the default.
  prefetch_ahead<int, 2>(arr.data(), /*cur=*/0, /*count=*/arr.size());
  prefetch_ahead(arr.data(), /*cur=*/0, /*count=*/0);  // empty
  SUCCEED();
}

TEST(CoreCpuHints, CacheLinePadHasCacheLineSize) {
  EXPECT_EQ(sizeof(CacheLinePad), kCacheLineSize);
}

TEST(CoreCpuHints, LikelyUnlikelyMacrosEvaluateCondition) {
  int x = 5;
  if (QBUEM_LIKELY(x == 5)) SUCCEED();
  else FAIL();
  if (QBUEM_UNLIKELY(x == 6)) FAIL();
  else SUCCEED();
}

// ===========================================================================
// session_store.hpp — tiny in-process mock implementing ISessionStore
// ===========================================================================

namespace {
// Minimal synchronous in-memory implementation. The exists() default in the
// interface delegates to get(); we keep it inherited to cover that default.
class MemorySessionStore : public ISessionStore {
 public:
  Task<Result<std::optional<std::string>>> get(
      std::string_view session_id) override {
    auto it = data_.find(std::string{session_id});
    if (it == data_.end()) co_return std::optional<std::string>{std::nullopt};
    co_return std::optional<std::string>{it->second};
  }

  Task<Result<void>> set(std::string_view session_id, std::string value,
                         std::chrono::seconds ttl =
                             std::chrono::seconds{3600}) override {
    (void)ttl;
    data_[std::string{session_id}] = std::move(value);
    co_return Result<void>{};
  }

  Task<Result<void>> del(std::string_view session_id) override {
    data_.erase(std::string{session_id});  // deleting absent key is fine
    co_return Result<void>{};
  }

  Task<Result<void>> touch(std::string_view session_id,
                           std::chrono::seconds ttl =
                               std::chrono::seconds{3600}) override {
    (void)ttl;
    if (data_.find(std::string{session_id}) == data_.end())
      co_return unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
    co_return Result<void>{};
  }

 private:
  std::unordered_map<std::string, std::string> data_;
};
}  // namespace

TEST(CoreSessionStore, SetThenGetReturnsValue) {
  MemorySessionStore store;
  auto sr = sync_value(store.set("sid-1", "payload"));
  ASSERT_TRUE(sr.has_value());

  auto gr = sync_value(store.get("sid-1"));
  ASSERT_TRUE(gr.has_value());        // Result has a value
  ASSERT_TRUE(gr.value().has_value()); // inner optional is engaged
  EXPECT_EQ(gr.value().value(), "payload");
}

TEST(CoreSessionStore, GetMissingReturnsNullopt) {
  MemorySessionStore store;
  auto gr = sync_value(store.get("absent"));
  ASSERT_TRUE(gr.has_value());
  EXPECT_FALSE(gr->has_value());
}

TEST(CoreSessionStore, DeleteRemovesSession) {
  MemorySessionStore store;
  sync_value(store.set("sid", "v"));
  auto dr = sync_value(store.del("sid"));
  ASSERT_TRUE(dr.has_value());
  auto gr = sync_value(store.get("sid"));
  ASSERT_TRUE(gr.has_value());
  EXPECT_FALSE(gr->has_value());
}

TEST(CoreSessionStore, DeleteAbsentIsNotAnError) {
  MemorySessionStore store;
  auto dr = sync_value(store.del("never-existed"));
  EXPECT_TRUE(dr.has_value());
}

TEST(CoreSessionStore, TouchExistingSucceeds) {
  MemorySessionStore store;
  sync_value(store.set("k", "x"));
  auto tr = sync_value(store.touch("k"));
  EXPECT_TRUE(tr.has_value());
}

TEST(CoreSessionStore, TouchMissingReturnsError) {
  MemorySessionStore store;
  auto tr = sync_value(store.touch("missing"));
  ASSERT_FALSE(tr.has_value());
  EXPECT_EQ(tr.error(),
            std::make_error_code(std::errc::no_such_file_or_directory));
}

TEST(CoreSessionStore, ExistsDefaultDelegatesToGet) {
  MemorySessionStore store;
  sync_value(store.set("present", "1"));

  auto e1 = sync_value(store.exists("present"));
  ASSERT_TRUE(e1.has_value());
  EXPECT_TRUE(e1.value());

  auto e2 = sync_value(store.exists("absent"));
  ASSERT_TRUE(e2.has_value());
  EXPECT_FALSE(e2.value());
}

// ===========================================================================
// transport.hpp + plain_transport.hpp
// ===========================================================================

TEST(CorePlainTransport, ConstructionStoresFd) {
  PlainTransport t{7};
  EXPECT_EQ(t.fd(), 7);
}

TEST(CorePlainTransport, HandshakeIsNoOpOk) {
  PlainTransport t{-1};
  auto r = sync_value(t.handshake());
  EXPECT_TRUE(r.has_value());
}

TEST(CorePlainTransport, EmptyReadReturnsZeroWithoutReactor) {
  // buf.empty() short-circuits before any AsyncRead suspension.
  PlainTransport t{-1};
  std::span<std::byte> empty{};
  auto r = sync_value(t.read(empty));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r.value(), 0u);
}

TEST(CorePlainTransport, EmptyWriteReturnsZeroWithoutReactor) {
  PlainTransport t{-1};
  std::span<const std::byte> empty{};
  auto r = sync_value(t.write(empty));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r.value(), 0u);
}

TEST(CorePlainTransport, CloseOnInvalidFdIsNoOpOk) {
  PlainTransport t{-1};
  auto r = sync_value(t.close());
  EXPECT_TRUE(r.has_value());
  EXPECT_EQ(t.fd(), -1);
}

TEST(CorePlainTransport, NegotiatedProtocolEmptyForPlain) {
  PlainTransport t{3};
  EXPECT_EQ(t.negotiated_protocol(), std::string_view{""});
  EXPECT_EQ(t.peer_certificate_fingerprint(), std::string_view{""});
}

TEST(CorePlainTransport, UsableThroughBasePointer) {
  std::unique_ptr<ITransport> t = std::make_unique<PlainTransport>(-1);
  auto r = sync_value(t->handshake());
  EXPECT_TRUE(r.has_value());
  auto c = sync_value(t->close());
  EXPECT_TRUE(c.has_value());
}

// ===========================================================================
// micro_ticker.hpp
// ===========================================================================

TEST(CoreMicroTicker, ConstructionExposesInterval) {
  MicroTicker ticker(std::chrono::microseconds{100});
  EXPECT_EQ(ticker.interval(), std::chrono::nanoseconds{100'000});
  EXPECT_FALSE(ticker.running());  // not started yet
}

TEST(CoreMicroTicker, RunsBoundedTicksThenStops) {
  // Interval comfortably larger than the spin phase so each tick is a short
  // coarse sleep + brief spin; correctness depends on the tick COUNT
  // (deterministic), and 5 ticks at 200us is ~1ms of wall time.
  MicroTicker ticker(std::chrono::microseconds{200},
                     std::chrono::microseconds{10});
  std::atomic<uint64_t> last_idx{0};
  int seen = 0;
  ticker.run([&](uint64_t idx) {
    last_idx.store(idx, std::memory_order_relaxed);
    if (++seen >= 5) ticker.stop();
  });
  EXPECT_EQ(seen, 5);
  EXPECT_EQ(last_idx.load(), 4u);     // indices 0..4
  EXPECT_FALSE(ticker.running());     // stop() flipped the flag
}

TEST(CoreMicroTicker, StopOnFirstTickYieldsExactlyOne) {
  // run() always re-arms running_=true at entry, so the only way to bound the
  // loop is to call stop() from inside the callback. Stopping on the first
  // tick must produce exactly one callback invocation (index 0).
  MicroTicker ticker(std::chrono::microseconds{200},
                     std::chrono::microseconds{10});
  int count = 0;
  uint64_t idx_seen = ~0ull;
  ticker.run([&](uint64_t idx) {
    idx_seen = idx;
    ++count;
    ticker.stop();
  });
  EXPECT_EQ(count, 1);
  EXPECT_EQ(idx_seen, 0u);
}

// ===========================================================================
// huge_pages.hpp
// ===========================================================================

TEST(CoreHugePages, AcquireReleaseCycle) {
  HugeBufferPool<4096, 4> pool;
  EXPECT_EQ(pool.capacity(), 4u);
  EXPECT_EQ(pool.available(), 4u);

  auto b0 = pool.acquire();
  ASSERT_FALSE(b0.empty());
  EXPECT_EQ(b0.size(), 4096u);
  EXPECT_EQ(pool.available(), 3u);

  // The buffer is writable.
  b0[0] = std::byte{0xAB};
  EXPECT_EQ(std::to_integer<int>(b0[0]), 0xAB);

  pool.release(b0);
  EXPECT_EQ(pool.available(), 4u);
}

TEST(CoreHugePages, ExhaustionReturnsEmptySpan) {
  HugeBufferPool<256, 2> pool;
  auto a = pool.acquire();
  auto b = pool.acquire();
  ASSERT_FALSE(a.empty());
  ASSERT_FALSE(b.empty());
  EXPECT_EQ(pool.available(), 0u);

  auto c = pool.acquire();  // exhausted
  EXPECT_TRUE(c.empty());

  pool.release(a);
  EXPECT_EQ(pool.available(), 1u);
  auto d = pool.acquire();  // now available again
  EXPECT_FALSE(d.empty());

  pool.release(b);
  pool.release(d);
}

TEST(CoreHugePages, ReleaseEmptySpanIsNoOp) {
  HugeBufferPool<128, 1> pool;
  std::span<std::byte> empty{};
  pool.release(empty);  // must be a no-op, not add a bogus slot
  EXPECT_EQ(pool.available(), 1u);
}

TEST(CoreHugePages, CapacityIsConstexpr) {
  static_assert(HugeBufferPool<512, 8>::capacity() == 8u);
  SUCCEED();
}

// ===========================================================================
// numa.hpp — graceful fallback on every platform
// ===========================================================================

TEST(CoreNuma, NodeCountIsAtLeastOne) {
  EXPECT_GE(numa_node_count(), 1);
}

TEST(CoreNuma, CpuToNumaMapSizeMatchesHardwareConcurrency) {
  auto m = cpu_to_numa_map();
  // Map size equals hardware_concurrency(); each entry is a valid node index.
  EXPECT_EQ(m.size(), static_cast<size_t>(std::thread::hardware_concurrency()));
  for (int node : m) {
    EXPECT_GE(node, 0);
    EXPECT_LT(node, numa_node_count());
  }
}

TEST(CoreNuma, PinOutOfRangeIndexReturnsFalse) {
  Dispatcher dispatcher(1);
  // Worker index past thread_count() must be rejected (false), never crash.
  EXPECT_FALSE(pin_reactor_to_cpu(dispatcher, /*idx=*/999, /*cpu=*/0));
}

TEST(CoreNuma, AutoNumaBindReturnsCleanly) {
  Dispatcher dispatcher(1);
  // On non-Linux this is a no-op returning 0; on Linux it returns a bound
  // count <= thread_count(). Either way it must return cleanly.
  size_t bound = auto_numa_bind(dispatcher);
  EXPECT_LE(bound, dispatcher.thread_count());
}

TEST(CorePerfCounters, AvailabilityAndSnapshotAreSafe) {
  PerfCounters pc;
  // available() reflects whether the PMU could be opened; on most CI/sandbox
  // environments and all non-Linux platforms this is false. Either value is OK.
  bool avail = pc.available();
  pc.start();
  auto snap = pc.stop();
  // ipc() must not divide by zero when cycles == 0 (graceful degradation).
  if (snap.cycles == 0) {
    EXPECT_DOUBLE_EQ(snap.ipc(), 0.0);
  } else {
    EXPECT_GE(snap.ipc(), 0.0);
  }
  (void)avail;
}
