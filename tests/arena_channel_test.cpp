#include <gtest/gtest.h>

#include <qbuem/pipeline/arena_channel.hpp>

#include <vector>

using namespace qbuem;

// ---------------------------------------------------------------------------
// ArenaChannel — PushPop
//
// Basic round-trip: push N items then pop them in FIFO order.
// ---------------------------------------------------------------------------
TEST(ArenaChannel, PushPopFifo) {
  ArenaChannel<int> chan(16);

  for (int i = 0; i < 8; ++i)
    EXPECT_TRUE(chan.push(i));

  for (int i = 0; i < 8; ++i) {
    auto v = chan.pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, i);
  }

  EXPECT_TRUE(chan.empty());
}

// ---------------------------------------------------------------------------
// ArenaChannel — PopEmpty
//
// pop() on an empty channel returns nullopt.
// ---------------------------------------------------------------------------
TEST(ArenaChannel, PopEmpty) {
  ArenaChannel<int> chan(4);
  EXPECT_FALSE(chan.pop().has_value());
}

// ---------------------------------------------------------------------------
// ArenaChannel — Backpressure
//
// push() returns false once the pool is exhausted.
// ---------------------------------------------------------------------------
TEST(ArenaChannel, Backpressure) {
  ArenaChannel<int> chan(4);

  EXPECT_TRUE(chan.push(1));
  EXPECT_TRUE(chan.push(2));
  EXPECT_TRUE(chan.push(3));
  EXPECT_TRUE(chan.push(4));
  EXPECT_FALSE(chan.push(5));   // pool full

  EXPECT_EQ(chan.size(), 4u);
  EXPECT_EQ(chan.available(), 0u);
}

// ---------------------------------------------------------------------------
// ArenaChannel — SlotReuse
//
// After popping, the freed slot can be reused immediately.
// ---------------------------------------------------------------------------
TEST(ArenaChannel, SlotReuse) {
  ArenaChannel<int> chan(2);

  EXPECT_TRUE(chan.push(10));
  EXPECT_TRUE(chan.push(20));
  EXPECT_FALSE(chan.push(30));  // full

  auto v = chan.pop();
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, 10);

  // Slot freed — can push again
  EXPECT_TRUE(chan.push(30));
  EXPECT_EQ(chan.size(), 2u);

  EXPECT_EQ(*chan.pop(), 20);
  EXPECT_EQ(*chan.pop(), 30);
  EXPECT_TRUE(chan.empty());
}

// ---------------------------------------------------------------------------
// ArenaChannel — PopBatch
//
// pop_batch() drains up to max_n items into a vector.
// ---------------------------------------------------------------------------
TEST(ArenaChannel, PopBatch) {
  ArenaChannel<int> chan(16);

  for (int i = 0; i < 10; ++i)
    chan.push(i);

  std::vector<int> out;
  size_t n = chan.pop_batch(out, 4);
  EXPECT_EQ(n, 4u);
  EXPECT_EQ(out.size(), 4u);
  EXPECT_EQ(out[0], 0);
  EXPECT_EQ(out[3], 3);

  EXPECT_EQ(chan.size(), 6u);
}

// ---------------------------------------------------------------------------
// ArenaChannel — PopBatchUnlimited
//
// pop_batch() with max_n=0 drains all items.
// ---------------------------------------------------------------------------
TEST(ArenaChannel, PopBatchUnlimited) {
  ArenaChannel<int> chan(16);

  for (int i = 0; i < 7; ++i)
    chan.push(i);

  std::vector<int> out;
  size_t n = chan.pop_batch(out);  // max_n=0 → all
  EXPECT_EQ(n, 7u);
  EXPECT_TRUE(chan.empty());
}

// ---------------------------------------------------------------------------
// ArenaChannel — Emplace
//
// emplace() constructs T in-place without an extra move.
// ---------------------------------------------------------------------------
TEST(ArenaChannel, Emplace) {
  ArenaChannel<std::string> chan(8);

  EXPECT_TRUE(chan.emplace("hello"));
  EXPECT_TRUE(chan.emplace(5, 'x'));   // string(count, char)

  auto a = chan.pop();
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(*a, "hello");

  auto b = chan.pop();
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(*b, "xxxxx");
}

// ---------------------------------------------------------------------------
// ArenaChannel — Capacity
//
// capacity() and available() track allocation state correctly.
// ---------------------------------------------------------------------------
TEST(ArenaChannel, CapacityTracking) {
  ArenaChannel<int> chan(8);

  EXPECT_EQ(chan.capacity(), 8u);
  EXPECT_EQ(chan.available(), 8u);

  chan.push(1);
  chan.push(2);
  EXPECT_EQ(chan.available(), 6u);

  chan.pop();
  EXPECT_EQ(chan.available(), 7u);
}

// ---------------------------------------------------------------------------
// ArenaChannel — MoveOnly
//
// ArenaChannel works with move-only types (unique_ptr).
// ---------------------------------------------------------------------------
TEST(ArenaChannel, MoveOnlyType) {
  ArenaChannel<std::unique_ptr<int>> chan(4);

  EXPECT_TRUE(chan.push(std::make_unique<int>(99)));
  EXPECT_TRUE(chan.push(std::make_unique<int>(42)));

  auto a = chan.pop();
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(**a, 99);

  auto b = chan.pop();
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(**b, 42);
}
