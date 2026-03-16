/**
 * @file tests/arena_test.cpp
 * @brief Unit tests for Arena (bump-pointer) and FixedPoolResource (free-list pool).
 */

#include <gtest/gtest.h>
#include <qbuem/core/arena.hpp>

#include <cstring>
#include <vector>

using namespace qbuem;

// ─── Arena ────────────────────────────────────────────────────────────────────

TEST(ArenaTest, AllocateReturnsNonNull) {
    Arena arena(4096);
    void* p = arena.allocate(128);
    EXPECT_NE(p, nullptr);
}

TEST(ArenaTest, SuccessiveAllocationsReturnDifferentPointers) {
    Arena arena(4096);
    void* p1 = arena.allocate(64);
    void* p2 = arena.allocate(64);
    EXPECT_NE(p1, p2);
}

TEST(ArenaTest, AllocatedMemoryIsWritable) {
    Arena arena(4096);
    auto* buf = static_cast<uint8_t*>(arena.allocate(256));
    std::memset(buf, 0xAB, 256);
    EXPECT_EQ(buf[0], 0xABu);
    EXPECT_EQ(buf[255], 0xABu);
}

TEST(ArenaTest, ResetAllowsReuse) {
    Arena arena(4096);
    void* p1 = arena.allocate(64);
    arena.reset();
    void* p2 = arena.allocate(64);
    // After reset, allocations start from the beginning again
    EXPECT_EQ(p1, p2);
}

TEST(ArenaTest, AlignmentIsRespected) {
    Arena arena(4096);

    // Allocate a 1-byte object to misalign the bump pointer
    arena.allocate(1);

    // Now request 8-byte aligned allocation
    void* p = arena.allocate(8, 8);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 8, 0u);
}

TEST(ArenaTest, LargeAllocationBeyondBlock) {
    Arena arena(64);  // Small initial block
    // Request more than the initial block — should allocate a new block
    void* p = arena.allocate(1024);
    EXPECT_NE(p, nullptr);
    // Should be writable
    std::memset(p, 0x77, 1024);
    EXPECT_EQ(static_cast<uint8_t*>(p)[0], 0x77u);
}

TEST(ArenaTest, MultipleResetCycles) {
    Arena arena(4096);
    for (int cycle = 0; cycle < 5; ++cycle) {
        auto* buf = static_cast<uint8_t*>(arena.allocate(512));
        std::memset(buf, static_cast<uint8_t>(cycle), 512);
        EXPECT_EQ(buf[0], static_cast<uint8_t>(cycle));
        arena.reset();
    }
}

// ─── FixedPoolResource ────────────────────────────────────────────────────────

// Use a struct >= sizeof(void*) = 8 bytes
struct alignas(8) Slot8 { uint64_t data; };
using Pool8 = FixedPoolResource<sizeof(Slot8), alignof(Slot8)>;

TEST(FixedPoolResourceTest, AllocateReturnsNonNull) {
    Pool8 pool(8);
    void* p = pool.allocate();
    EXPECT_NE(p, nullptr);
}

TEST(FixedPoolResourceTest, CapacityMatchesConstructorArg) {
    Pool8 pool(16);
    EXPECT_EQ(pool.capacity(), 16u);
}

TEST(FixedPoolResourceTest, UsedAndAvailableTrackCorrectly) {
    Pool8 pool(4);
    EXPECT_EQ(pool.used(), 0u);
    EXPECT_EQ(pool.available(), 4u);

    void* p = pool.allocate();
    EXPECT_EQ(pool.used(), 1u);
    EXPECT_EQ(pool.available(), 3u);

    pool.deallocate(p);
    EXPECT_EQ(pool.used(), 0u);
    EXPECT_EQ(pool.available(), 4u);
}

TEST(FixedPoolResourceTest, OverflowReturnsNullptr) {
    Pool8 pool(2);
    pool.allocate();
    pool.allocate();
    // Pool exhausted
    void* overflow = pool.allocate();
    EXPECT_EQ(overflow, nullptr);
}

TEST(FixedPoolResourceTest, DeallocatedSlotIsReused) {
    Pool8 pool(2);
    void* p1 = pool.allocate();
    pool.allocate();
    pool.deallocate(p1);

    // Next allocate should reuse p1 (LIFO free-list)
    void* p3 = pool.allocate();
    EXPECT_EQ(p1, p3);
}

TEST(FixedPoolResourceTest, AllocatedMemoryIsWritable) {
    Pool8 pool(4);
    auto* s = static_cast<Slot8*>(pool.allocate());
    ASSERT_NE(s, nullptr);
    s->data = 0xDEADBEEFCAFEBABEull;
    EXPECT_EQ(s->data, 0xDEADBEEFCAFEBABEull);
    pool.deallocate(s);
}

TEST(FixedPoolResourceTest, AllSlotsAllocatable) {
    Pool8 pool(32);
    std::vector<void*> slots;
    while (auto* p = pool.allocate())
        slots.push_back(p);

    EXPECT_EQ(slots.size(), 32u);
    EXPECT_EQ(pool.used(), 32u);
    EXPECT_EQ(pool.available(), 0u);

    for (auto* p : slots)
        pool.deallocate(p);

    EXPECT_EQ(pool.available(), 32u);
}
