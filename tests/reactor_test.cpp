#include <qbuem/common.hpp>
#include <qbuem/core/arena.hpp>
#include <qbuem/core/task.hpp>
#include <gtest/gtest.h>

TEST(InfraTest, BasicStructure) {
  // Verify common types are accessible
  qbuem::Result<int> res = 42;
  EXPECT_TRUE(res.has_value());
  EXPECT_EQ(*res, 42);
}

qbuem::Task<void> sample_coro(int &value) {
  value = 100;
  co_return;
}

TEST(InfraTest, CoroutineTask) {
  int v = 0;
  {
    auto task = sample_coro(v);
    EXPECT_EQ(v, 0); // Not resumed yet
    task.resume();
    EXPECT_EQ(v, 100);
  }
}

TEST(InfraTest, ArenaAllocation) {
  qbuem::Arena arena(1024);

  void *p1 = arena.allocate(128);
  void *p2 = arena.allocate(256);

  EXPECT_NE(p1, nullptr);
  EXPECT_NE(p2, nullptr);
  EXPECT_NE(p1, p2);

  // Test alignment
  void *p3 = arena.allocate(8, 64);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p3) % 64, 0);

  arena.reset();
  void *p4 = arena.allocate(128);
  // After reset, p4 should probably reuse p1's address (or start of block)
  EXPECT_EQ(p1, p4);
}
