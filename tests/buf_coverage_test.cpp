// SPDX-License-Identifier: MIT
/**
 * @file tests/buf_coverage_test.cpp
 * @brief Additional coverage for the qbuem::buf module group.
 *
 * Targets four under-tested buffer headers. The bulk of the new coverage is
 * for IntrusiveList and KqueueBufferPool (no prior tests exist). For
 * GenerationPool and LockFreeHashMap this file only adds edge cases that
 * buf_pools_test.cpp does NOT already exercise (capacity getters, full-map
 * probing, boundary capacities, missing-key remove, double-release no-op,
 * mutated/forged handles, etc.).
 *
 * All API names/signatures are copied verbatim from the headers:
 *   - include/qbuem/buf/intrusive_list.hpp     (IntrusiveNode, IntrusiveList<T>)
 *   - include/qbuem/buf/kqueue_buffer_pool.hpp (KqueueBufferPool, ::Buffer)
 *   - include/qbuem/buf/generation_pool.hpp    (GenerationPool<T>, GenerationHandle)
 *   - include/qbuem/buf/lock_free_hash_map.hpp (LockFreeHashMap<K,V>)
 *
 * Errors are std::expected (Result<T>); both value and error paths are checked.
 */

#include <gtest/gtest.h>

#include <qbuem/buf/intrusive_list.hpp>
#include <qbuem/buf/kqueue_buffer_pool.hpp>
#include <qbuem/buf/generation_pool.hpp>
#include <qbuem/buf/lock_free_hash_map.hpp>

#include <cstdint>
#include <cstring>
#include <new>
#include <system_error>
#include <vector>

using namespace qbuem;

// ───────────────────────── IntrusiveList ─────────────────────────────────────

namespace {

struct Node : public IntrusiveNode {
    int value{0};
    explicit Node(int v = 0) : value(v) {}
};

} // namespace

TEST(IntrusiveListTest, DefaultConstructedIsEmpty) {
    IntrusiveList<Node> list;
    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.size(), 0u);
    EXPECT_EQ(list.begin(), list.end());
}

TEST(IntrusiveListTest, NodeStartsUnlinked) {
    Node n{1};
    EXPECT_FALSE(n.linked());
}

TEST(IntrusiveListTest, PushBackPreservesOrder) {
    IntrusiveList<Node> list;
    Node a{1}, b{2}, c{3};
    list.push_back(&a);
    list.push_back(&b);
    list.push_back(&c);

    EXPECT_FALSE(list.empty());
    EXPECT_EQ(list.size(), 3u);
    EXPECT_EQ(list.front()->value, 1);
    EXPECT_EQ(list.back()->value, 3);
    EXPECT_TRUE(a.linked());

    std::vector<int> seen;
    for (Node& n : list) seen.push_back(n.value);
    EXPECT_EQ(seen, (std::vector<int>{1, 2, 3}));

    // Clean up before nodes go out of scope (header lifetime contract).
    list.remove(&a);
    list.remove(&b);
    list.remove(&c);
}

TEST(IntrusiveListTest, PushFrontReversesInsertionOrder) {
    IntrusiveList<Node> list;
    Node a{1}, b{2}, c{3};
    list.push_front(&a);
    list.push_front(&b);
    list.push_front(&c);

    EXPECT_EQ(list.size(), 3u);
    std::vector<int> seen;
    for (Node& n : list) seen.push_back(n.value);
    EXPECT_EQ(seen, (std::vector<int>{3, 2, 1}));
    EXPECT_EQ(list.front()->value, 3);
    EXPECT_EQ(list.back()->value, 1);

    while (!list.empty()) (void)list.pop_front();
}

TEST(IntrusiveListTest, RemoveMiddleNode) {
    IntrusiveList<Node> list;
    Node a{1}, b{2}, c{3};
    list.push_back(&a);
    list.push_back(&b);
    list.push_back(&c);

    list.remove(&b);
    EXPECT_FALSE(b.linked());
    EXPECT_EQ(list.size(), 2u);

    std::vector<int> seen;
    for (Node& n : list) seen.push_back(n.value);
    EXPECT_EQ(seen, (std::vector<int>{1, 3}));

    list.remove(&a);
    list.remove(&c);
    EXPECT_TRUE(list.empty());
}

TEST(IntrusiveListTest, PopFrontAndPopBackReturnEndpoints) {
    IntrusiveList<Node> list;
    Node a{10}, b{20}, c{30};
    list.push_back(&a);
    list.push_back(&b);
    list.push_back(&c);

    Node* f = list.pop_front();
    EXPECT_EQ(f, &a);
    EXPECT_FALSE(a.linked());
    EXPECT_EQ(list.size(), 2u);

    Node* bk = list.pop_back();
    EXPECT_EQ(bk, &c);
    EXPECT_FALSE(c.linked());
    EXPECT_EQ(list.size(), 1u);
    EXPECT_EQ(list.front(), &b);

    (void)list.pop_back();
    EXPECT_TRUE(list.empty());
}

TEST(IntrusiveListTest, SingleElementFrontEqualsBack) {
    IntrusiveList<Node> list;
    Node only{7};
    list.push_back(&only);
    EXPECT_EQ(list.front(), list.back());
    EXPECT_EQ(list.front()->value, 7);
    EXPECT_EQ(list.size(), 1u);
    list.remove(&only);
}

TEST(IntrusiveListTest, BidirectionalIteration) {
    IntrusiveList<Node> list;
    Node a{1}, b{2}, c{3};
    list.push_back(&a);
    list.push_back(&b);
    list.push_back(&c);

    auto it = list.begin();
    EXPECT_EQ(it->value, 1);
    ++it;
    EXPECT_EQ(it->value, 2);
    auto post = it++;          // post-increment returns old position
    EXPECT_EQ(post->value, 2);
    EXPECT_EQ(it->value, 3);
    --it;                      // step back
    EXPECT_EQ(it->value, 2);

    while (!list.empty()) (void)list.pop_front();
}

TEST(IntrusiveListTest, ConstIteration) {
    IntrusiveList<Node> list;
    Node a{5}, b{6};
    list.push_back(&a);
    list.push_back(&b);

    const IntrusiveList<Node>& cref = list;
    int sum = 0;
    for (auto it = cref.cbegin(); it != cref.cend(); ++it) sum += it->value;
    EXPECT_EQ(sum, 11);
    EXPECT_EQ(cref.front()->value, 5);
    EXPECT_EQ(cref.back()->value, 6);

    list.remove(&a);
    list.remove(&b);
}

TEST(IntrusiveListTest, MoveConstructTransfersNodes) {
    IntrusiveList<Node> src;
    Node a{1}, b{2}, c{3};
    src.push_back(&a);
    src.push_back(&b);
    src.push_back(&c);

    IntrusiveList<Node> dst(std::move(src));
    EXPECT_EQ(dst.size(), 3u);
    EXPECT_TRUE(src.empty());            // moved-from list is empty
    EXPECT_EQ(dst.front()->value, 1);
    EXPECT_EQ(dst.back()->value, 3);

    std::vector<int> seen;
    for (Node& n : dst) seen.push_back(n.value);
    EXPECT_EQ(seen, (std::vector<int>{1, 2, 3}));

    while (!dst.empty()) (void)dst.pop_front();
}

TEST(IntrusiveListTest, MoveAssignFromEmptyLeavesTargetEmpty) {
    IntrusiveList<Node> src;       // empty
    IntrusiveList<Node> dst;
    Node a{1};
    dst.push_back(&a);
    dst.remove(&a);                // dst empty again, then assign empty src

    dst = std::move(src);
    EXPECT_TRUE(dst.empty());
    EXPECT_EQ(dst.size(), 0u);
}

TEST(IntrusiveListTest, MoveAssignSelfIsNoop) {
    IntrusiveList<Node> list;
    Node a{1}, b{2};
    list.push_back(&a);
    list.push_back(&b);

    IntrusiveList<Node>& alias = list;
    list = std::move(alias);       // self-move guarded in header
    EXPECT_EQ(list.size(), 2u);
    EXPECT_EQ(list.front()->value, 1);

    list.remove(&a);
    list.remove(&b);
}

TEST(IntrusiveListTest, RemoveResetsNodePointers) {
    IntrusiveList<Node> list;
    Node a{1};
    list.push_back(&a);
    EXPECT_TRUE(a.linked());
    list.remove(&a);
    EXPECT_FALSE(a.linked());
    // After removal the node may be re-pushed into any list.
    IntrusiveList<Node> other;
    other.push_back(&a);
    EXPECT_EQ(other.front(), &a);
    other.remove(&a);
}

// ───────────────────────── KqueueBufferPool ─────────────────────────────────

TEST(KqueueBufferPoolTest, ConstructionRoundsBufferSizeUpToCacheLine) {
    KqueueBufferPool pool(100, 4);   // 100 -> rounded to 128
    EXPECT_EQ(pool.available(), 4u);

    KqueueBufferPool::Buffer b = pool.acquire();
    ASSERT_NE(b.addr, nullptr);
    EXPECT_EQ(b.len, 128u);           // (100 + 63) & ~63 == 128
    pool.release(b.bid);
}

TEST(KqueueBufferPoolTest, ExactMultipleSizeUnchanged) {
    KqueueBufferPool pool(64, 2);
    KqueueBufferPool::Buffer b = pool.acquire();
    ASSERT_NE(b.addr, nullptr);
    EXPECT_EQ(b.len, 64u);
    pool.release(b.bid);
}

TEST(KqueueBufferPoolTest, AcquireDecrementsAvailable) {
    KqueueBufferPool pool(128, 3);
    EXPECT_EQ(pool.available(), 3u);
    KqueueBufferPool::Buffer b1 = pool.acquire();
    EXPECT_EQ(pool.available(), 2u);
    KqueueBufferPool::Buffer b2 = pool.acquire();
    EXPECT_EQ(pool.available(), 1u);
    ASSERT_NE(b1.addr, nullptr);
    ASSERT_NE(b2.addr, nullptr);
    EXPECT_NE(b1.bid, b2.bid);       // distinct buffer ids
    EXPECT_NE(b1.addr, b2.addr);     // distinct addresses
    pool.release(b1.bid);
    pool.release(b2.bid);
    EXPECT_EQ(pool.available(), 3u);
}

TEST(KqueueBufferPoolTest, ExhaustionReturnsNullAddr) {
    KqueueBufferPool pool(64, 2);
    KqueueBufferPool::Buffer b1 = pool.acquire();
    KqueueBufferPool::Buffer b2 = pool.acquire();
    ASSERT_NE(b1.addr, nullptr);
    ASSERT_NE(b2.addr, nullptr);
    EXPECT_EQ(pool.available(), 0u);

    KqueueBufferPool::Buffer empty = pool.acquire();   // pool drained
    EXPECT_EQ(empty.addr, nullptr);
    EXPECT_EQ(empty.len, 0u);
    EXPECT_EQ(empty.bid, 0u);

    pool.release(b1.bid);
    EXPECT_EQ(pool.available(), 1u);
    KqueueBufferPool::Buffer b3 = pool.acquire();      // re-acquire after release
    EXPECT_NE(b3.addr, nullptr);
    pool.release(b2.bid);
    pool.release(b3.bid);
}

TEST(KqueueBufferPoolTest, AcquiredMemoryIsWritableAndAligned) {
    KqueueBufferPool pool(256, 2);
    KqueueBufferPool::Buffer b = pool.acquire();
    ASSERT_NE(b.addr, nullptr);
    // 64-byte aligned per posix_memalign.
    EXPECT_EQ(reinterpret_cast<uintptr_t>(b.addr) % 64u, 0u);
    // Fully writable for the reported length.
    std::memset(b.addr, 0xAB, b.len);
    EXPECT_EQ(*static_cast<uint8_t*>(b.addr), 0xABu);
    pool.release(b.bid);
}

TEST(KqueueBufferPoolTest, ReleaseRestoresFullAvailability) {
    KqueueBufferPool pool(64, 5);
    std::vector<KqueueBufferPool::Buffer> held;
    for (int i = 0; i < 5; ++i) {
        KqueueBufferPool::Buffer b = pool.acquire();
        ASSERT_NE(b.addr, nullptr);
        held.push_back(b);
    }
    EXPECT_EQ(pool.available(), 0u);
    for (auto& b : held) pool.release(b.bid);
    EXPECT_EQ(pool.available(), 5u);
}

// ───────────────────────── GenerationPool (new edge cases) ───────────────────

namespace {
struct PoolObj {
    int    a{0};
    double b{0.0};
};
} // namespace

TEST(GenerationPoolCovTest, CapacityGetterReflectsConstruction) {
    GenerationPool<PoolObj> pool(7);
    EXPECT_EQ(pool.capacity(), 7u);
}

TEST(GenerationPoolCovTest, SingleSlotCapacity) {
    GenerationPool<PoolObj> pool(1);
    EXPECT_EQ(pool.capacity(), 1u);
    auto r = pool.acquire();
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->handle.valid());
    // Second acquire on a single-slot pool must be exhausted.
    auto r2 = pool.acquire();
    EXPECT_FALSE(r2.has_value());
    pool.release(r->handle);
}

TEST(GenerationPoolCovTest, PlacementConstructAndResolveRoundTrip) {
    GenerationPool<PoolObj> pool(4);
    auto r = pool.acquire();
    ASSERT_TRUE(r.has_value());
    PoolObj* obj = new (r->ptr) PoolObj{42, 3.5};
    PoolObj* resolved = pool.resolve(r->handle);
    ASSERT_NE(resolved, nullptr);
    EXPECT_EQ(resolved, obj);
    EXPECT_EQ(resolved->a, 42);
    EXPECT_DOUBLE_EQ(resolved->b, 3.5);
    obj->~PoolObj();
    pool.release(r->handle);
}

TEST(GenerationPoolCovTest, DoubleReleaseIsNoop) {
    GenerationPool<PoolObj> pool(3);
    auto r = pool.acquire();
    ASSERT_TRUE(r.has_value());
    GenerationHandle h = r->handle;
    pool.release(h);
    // Second release of the same (now stale) handle is a safe no-op:
    // generation no longer matches, so nothing is pushed back.
    pool.release(h);
    EXPECT_EQ(pool.resolve(h), nullptr);

    // The pool still has its full capacity (no slot leaked or double-pushed):
    std::vector<GenerationHandle> handles;
    for (size_t i = 0; i < pool.capacity(); ++i) {
        auto a = pool.acquire();
        ASSERT_TRUE(a.has_value());
        handles.push_back(a->handle);
    }
    EXPECT_FALSE(pool.acquire().has_value());  // exhausted, no duplicates
    for (auto hh : handles) pool.release(hh);
}

TEST(GenerationPoolCovTest, ResolveOutOfRangeIndexHandleReturnsNull) {
    GenerationPool<PoolObj> pool(2);
    // Forge a handle whose index is beyond capacity.
    GenerationHandle bad{/*index=*/99, /*gen=*/0};
    EXPECT_EQ(pool.resolve(bad), nullptr);
}

TEST(GenerationPoolCovTest, ResolveWrongGenerationReturnsNull) {
    GenerationPool<PoolObj> pool(2);
    auto r = pool.acquire();
    ASSERT_TRUE(r.has_value());
    uint32_t idx = r->handle.index();
    // Same valid index but a deliberately wrong generation -> stale.
    GenerationHandle forged{idx, r->handle.gen() + 100u};
    EXPECT_EQ(pool.resolve(forged), nullptr);
    // The genuine handle still resolves.
    EXPECT_NE(pool.resolve(r->handle), nullptr);
    pool.release(r->handle);
}

TEST(GenerationPoolCovTest, ReleaseNullHandleIsSafe) {
    GenerationPool<PoolObj> pool(2);
    GenerationHandle null_handle;          // default = invalid
    EXPECT_FALSE(null_handle.valid());
    pool.release(null_handle);             // must not touch any slot
    // Both slots remain acquirable.
    auto a = pool.acquire();
    auto b = pool.acquire();
    EXPECT_TRUE(a.has_value());
    EXPECT_TRUE(b.has_value());
    if (a) pool.release(a->handle);
    if (b) pool.release(b->handle);
}

TEST(GenerationPoolCovTest, HandleEncodingRoundTrip) {
    GenerationHandle h{12345u, 678u};
    EXPECT_EQ(h.index(), 12345u);
    EXPECT_EQ(h.gen(), 678u);
    EXPECT_TRUE(h.valid());
    GenerationHandle same{12345u, 678u};
    EXPECT_EQ(h, same);                      // operator== defaulted
    GenerationHandle different{12345u, 679u};
    EXPECT_NE(h, different);
}

// ───────────────────────── LockFreeHashMap (new edge cases) ──────────────────

TEST(LockFreeHashMapCovTest, MinimumCapacityIsRespected) {
    LockFreeHashMap<uint64_t, uint32_t> map(2);
    EXPECT_EQ(map.capacity(), 2u);   // already power of two
}

TEST(LockFreeHashMapCovTest, NonPowerOfTwoRoundsUp) {
    LockFreeHashMap<uint64_t, uint32_t> map(5);
    EXPECT_EQ(map.capacity(), 8u);   // bit_ceil(5) == 8
}

TEST(LockFreeHashMapCovTest, FullMapPutReturnsFalse) {
    LockFreeHashMap<uint64_t, uint32_t> map(2);   // capacity exactly 2
    EXPECT_TRUE(map.put(1, 100));
    EXPECT_TRUE(map.put(2, 200));
    // Map is now full; inserting a third distinct key must fail.
    EXPECT_FALSE(map.put(3, 300));
    // Existing keys are still retrievable.
    auto v1 = map.get(1);
    auto v2 = map.get(2);
    ASSERT_TRUE(v1.has_value());
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(*v1, 100u);
    EXPECT_EQ(*v2, 200u);
    // The failed key is absent.
    EXPECT_FALSE(map.get(3).has_value());
}

TEST(LockFreeHashMapCovTest, UpdateInFullMapStillSucceeds) {
    LockFreeHashMap<uint64_t, uint32_t> map(2);
    EXPECT_TRUE(map.put(1, 10));
    EXPECT_TRUE(map.put(2, 20));
    // Updating an existing key on a full map is allowed (in-place store).
    EXPECT_TRUE(map.put(1, 11));
    auto v = map.get(1);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 11u);
}

TEST(LockFreeHashMapCovTest, GetMissingReturnsNoSuchFileError) {
    LockFreeHashMap<uint64_t, uint32_t> map(16);
    auto r = map.get(777);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), std::make_error_code(std::errc::no_such_file_or_directory));
}

TEST(LockFreeHashMapCovTest, RemoveMissingKeyReturnsFalse) {
    LockFreeHashMap<uint64_t, uint32_t> map(16);
    EXPECT_FALSE(map.remove(42));          // key never inserted
    EXPECT_TRUE(map.put(42, 1));
    EXPECT_TRUE(map.remove(42));           // now present
    EXPECT_FALSE(map.remove(42));          // already removed -> false
}

TEST(LockFreeHashMapCovTest, GetAfterRemoveReportsMissing) {
    LockFreeHashMap<uint64_t, uint32_t> map(16);
    EXPECT_TRUE(map.put(9, 99));
    ASSERT_TRUE(map.get(9).has_value());
    EXPECT_TRUE(map.remove(9));
    EXPECT_FALSE(map.get(9).has_value()); // tombstoned slot is not a hit
}

TEST(LockFreeHashMapCovTest, PointerSizedValueType) {
    // V must be <=8 bytes; a raw pointer qualifies and is trivially copyable.
    int target = 1234;
    LockFreeHashMap<uint64_t, int*> map(16);
    EXPECT_TRUE(map.put(1, &target));
    auto v = map.get(1);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, &target);
    EXPECT_EQ(**v, 1234);
}

TEST(LockFreeHashMapCovTest, FillToExactCapacityAllSucceed) {
    LockFreeHashMap<uint64_t, uint64_t> map(8);   // capacity 8
    for (uint64_t k = 1; k <= 8; ++k) {
        EXPECT_TRUE(map.put(k, k * 10));
    }
    // The 9th distinct key must fail (capacity full).
    EXPECT_FALSE(map.put(9, 90));
    // All eight inserted keys still resolve.
    for (uint64_t k = 1; k <= 8; ++k) {
        auto v = map.get(k);
        ASSERT_TRUE(v.has_value()) << "key " << k;
        EXPECT_EQ(*v, k * 10);
    }
}

TEST(LockFreeHashMapCovTest, RemoveFreesSlotForNewKeyInFullMap) {
    LockFreeHashMap<uint64_t, uint32_t> map(2);
    EXPECT_TRUE(map.put(1, 1));
    EXPECT_TRUE(map.put(2, 2));
    EXPECT_FALSE(map.put(3, 3));   // full
    EXPECT_TRUE(map.remove(1));    // free a slot (tombstone reusable by put)
    EXPECT_TRUE(map.put(3, 3));    // tombstone reclaimed
    auto v = map.get(3);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 3u);
}
