#include <qbuem/pipeline/retry_policy.hpp>
#include <qbuem/pipeline/circuit_breaker.hpp>
#include <qbuem/pipeline/dead_letter.hpp>
#include <qbuem/pipeline/priority_channel.hpp>
#include <qbuem/pipeline/spsc_channel.hpp>
#include <gtest/gtest.h>
using namespace qbuem;
using namespace std::chrono_literals;

// --- RetryConfig ---
TEST(RetryConfig, Defaults) {
  RetryConfig cfg;
  EXPECT_EQ(cfg.max_attempts, 3u);
  EXPECT_EQ(cfg.base_delay, 100ms);
  EXPECT_EQ(cfg.strategy, BackoffStrategy::Exponential);
}

// --- CircuitBreaker ---
TEST(CircuitBreaker, InitiallyClosed) {
  CircuitBreaker cb;
  EXPECT_EQ(cb.state(), CircuitBreaker::State::Closed);
  EXPECT_TRUE(cb.allow_request());
}

TEST(CircuitBreaker, OpensAfterFailures) {
  CircuitBreaker cb({.failure_threshold=3, .timeout=100ms});
  cb.record_failure();
  cb.record_failure();
  EXPECT_EQ(cb.state(), CircuitBreaker::State::Closed);
  cb.record_failure(); // threshold hit
  EXPECT_EQ(cb.state(), CircuitBreaker::State::Open);
  EXPECT_FALSE(cb.allow_request()); // fast fail
}

TEST(CircuitBreaker, Reset) {
  CircuitBreaker cb({.failure_threshold=2});
  cb.record_failure(); cb.record_failure();
  EXPECT_EQ(cb.state(), CircuitBreaker::State::Open);
  cb.reset();
  EXPECT_EQ(cb.state(), CircuitBreaker::State::Closed);
  EXPECT_TRUE(cb.allow_request());
}

TEST(CircuitBreaker, SuccessCounterResets) {
  CircuitBreaker cb;
  cb.record_failure();
  cb.record_success(); // success resets failure counter
  EXPECT_EQ(cb.failure_count(), 0u);
}

TEST(CircuitBreaker, OnStateChangeCallback) {
  int called = 0;
  CircuitBreaker::State from_state, to_state;
  CircuitBreaker cb({
    .failure_threshold=1,
    .on_state_change=[&](auto from, auto to){ ++called; from_state=from; to_state=to; }
  });
  cb.record_failure();
  EXPECT_EQ(called, 1);
  EXPECT_EQ(to_state, CircuitBreaker::State::Open);
}

// --- DeadLetterQueue ---
TEST(DeadLetterQueue, PushAndSize) {
  DeadLetterQueue<int> dlq;
  dlq.push(42, {}, std::make_error_code(std::errc::timed_out));
  EXPECT_EQ(dlq.size(), 1u);
  EXPECT_FALSE(dlq.empty());
}

TEST(DeadLetterQueue, Drain) {
  DeadLetterQueue<int> dlq;
  dlq.push(1, {}, {});
  dlq.push(2, {}, {});
  auto letters = dlq.drain();
  EXPECT_EQ(letters.size(), 2u);
  EXPECT_EQ(dlq.size(), 0u);
  EXPECT_TRUE(dlq.empty());
}

TEST(DeadLetterQueue, MaxSizeDropsOldest) {
  DeadLetterQueue<int> dlq({.max_size=3});
  for (int i = 0; i < 5; ++i)
    dlq.push(i, {}, {});
  EXPECT_EQ(dlq.size(), 3u);
  auto letters = dlq.drain();
  EXPECT_EQ(letters[0].item, 2); // oldest 0,1 dropped
}

// --- PriorityChannel ---
TEST(PriorityChannel, SendRecvByPriority) {
  PriorityChannel<int> chan(8);
  chan.try_send(1, Priority::Low);
  chan.try_send(2, Priority::High);
  chan.try_send(3, Priority::Normal);

  auto a = chan.try_recv(); ASSERT_TRUE(a); EXPECT_EQ(*a, 2); // High first
  auto b = chan.try_recv(); ASSERT_TRUE(b); EXPECT_EQ(*b, 3); // Normal
  auto c = chan.try_recv(); ASSERT_TRUE(c); EXPECT_EQ(*c, 1); // Low last
}

TEST(PriorityChannel, CloseAndEOS) {
  PriorityChannel<int> chan(4);
  chan.close();
  EXPECT_TRUE(chan.is_closed());
  EXPECT_FALSE(chan.try_recv().has_value());
}

TEST(PriorityChannel, SizeApprox) {
  PriorityChannel<int> chan(8);
  chan.try_send(1, Priority::High);
  chan.try_send(2, Priority::Normal);
  EXPECT_EQ(chan.size_approx(), 2u);
  EXPECT_EQ(chan.size_approx(Priority::High), 1u);
  EXPECT_EQ(chan.size_approx(Priority::Normal), 1u);
  EXPECT_EQ(chan.size_approx(Priority::Low), 0u);
}

// --- SpscChannel ---
TEST(SpscChannel, TrySendRecv) {
  SpscChannel<int> chan(8);
  EXPECT_TRUE(chan.try_send(42));
  auto v = chan.try_recv();
  ASSERT_TRUE(v); EXPECT_EQ(*v, 42);
}

TEST(SpscChannel, TryRecvEmpty) {
  SpscChannel<int> chan(4);
  EXPECT_FALSE(chan.try_recv().has_value());
}

TEST(SpscChannel, TrySendFull) {
  SpscChannel<int> chan(4);
  for (int i = 0; i < 4; ++i)
    EXPECT_TRUE(chan.try_send(i));
  EXPECT_FALSE(chan.try_send(99)); // full
}

TEST(SpscChannel, CapacityRoundsUpToPowerOfTwo) {
  SpscChannel<int> chan(5); // rounds to 8
  EXPECT_EQ(chan.capacity(), 8u);
}

TEST(SpscChannel, Close) {
  SpscChannel<int> chan(4);
  chan.try_send(1);
  chan.close();
  EXPECT_TRUE(chan.is_closed());
  auto v = chan.try_recv(); // still can recv remaining
  ASSERT_TRUE(v); EXPECT_EQ(*v, 1);
}

TEST(SpscChannel, OrderPreserved) {
  SpscChannel<int> chan(16);
  for (int i = 0; i < 10; ++i) chan.try_send(i);
  for (int i = 0; i < 10; ++i) {
    auto v = chan.try_recv();
    ASSERT_TRUE(v); EXPECT_EQ(*v, i);
  }
}
