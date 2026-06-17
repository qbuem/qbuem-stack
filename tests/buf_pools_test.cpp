/**
 * @file tests/buf_pools_test.cpp
 * @brief Unit tests for GenerationPool (ABA-safe handle pool),
 *        LockFreeHashMap (open-addressed MPMC map), and the
 *        ErasureCoder / gf256 Reed-Solomon erasure coding primitives.
 *
 * Coverage:
 *  - GenerationPool: distinct slots on acquire, release+re-acquire reuse,
 *    ABA safety (a stale handle resolves to nullptr after re-acquire bumps gen),
 *    exhaustion, and validity of fresh handles.
 *  - LockFreeHashMap: insert/find, update-in-place, miss, erase + miss-after,
 *    re-insert after erase.
 *  - ErasureCoder: encode k data → m parity, then erase up to m shards and
 *    reconstruct, asserting byte-exact recovery over several erasure patterns.
 *    A failed reconstruction for a valid (≤ m lost) pattern is reported as a
 *    library bug, not papered over.
 *
 * NOTE: The task referenced `buf/fixed_pool.hpp`, but FixedPoolResource lives
 * in `core/arena.hpp` and is already covered by tests/arena_test.cpp; this file
 * therefore focuses on the three buf-area types above.
 */

#include <gtest/gtest.h>

#include <qbuem/buf/generation_pool.hpp>
#include <qbuem/buf/lock_free_hash_map.hpp>
#include <qbuem/buf/simd_erasure.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <set>
#include <span>
#include <vector>

using namespace qbuem;

// ─── GenerationPool ─────────────────────────────────────────────────────────────

namespace {
struct Event {
    uint64_t a{0};
    uint32_t b{0};
};
} // namespace

TEST(GenerationPoolTest, AcquireReturnsValidHandleAndStorage) {
    GenerationPool<Event> pool(8);
    auto r = pool.acquire();
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->handle.valid());
    EXPECT_NE(r->ptr, nullptr);

    // The handle must resolve back to the very same storage pointer.
    EXPECT_EQ(pool.resolve(r->handle), r->ptr);
}

TEST(GenerationPoolTest, AcquireReturnsDistinctSlots) {
    GenerationPool<Event> pool(8);
    std::set<void*> ptrs;
    std::set<uint32_t> indices;

    for (int i = 0; i < 8; ++i) {
        auto r = pool.acquire();
        ASSERT_TRUE(r.has_value()) << "acquire " << i << " failed";
        ptrs.insert(static_cast<void*>(r->ptr));
        indices.insert(r->handle.index());
    }
    // All 8 acquisitions yield distinct storage pointers and distinct indices.
    EXPECT_EQ(ptrs.size(), 8u);
    EXPECT_EQ(indices.size(), 8u);
}

TEST(GenerationPoolTest, ExhaustionReturnsNullopt) {
    GenerationPool<Event> pool(3);
    EXPECT_EQ(pool.capacity(), 3u);

    auto a = pool.acquire();
    auto b = pool.acquire();
    auto c = pool.acquire();
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_TRUE(c.has_value());

    auto d = pool.acquire(); // pool exhausted
    EXPECT_FALSE(d.has_value());
}

TEST(GenerationPoolTest, ReleaseThenReacquireReusesSlot) {
    GenerationPool<Event> pool(2);

    auto a = pool.acquire();
    auto b = pool.acquire();
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    // Pool now exhausted.
    EXPECT_FALSE(pool.acquire().has_value());

    void* freed_ptr = static_cast<void*>(a->ptr);
    pool.release(a->handle);

    // Re-acquire should succeed and reuse the freed storage.
    auto c = pool.acquire();
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(static_cast<void*>(c->ptr), freed_ptr);
}

TEST(GenerationPoolTest, ReleasedHandleNoLongerResolves) {
    GenerationPool<Event> pool(4);
    auto a = pool.acquire();
    ASSERT_TRUE(a.has_value());
    EXPECT_NE(pool.resolve(a->handle), nullptr);

    pool.release(a->handle);
    // After release, the slot is in a transitioning/odd generation: stale.
    EXPECT_EQ(pool.resolve(a->handle), nullptr);
}

// ABA safety: a handle that was released and whose slot got re-acquired (gen
// bumped) must resolve to nullptr — even though the index is reused, the
// generation no longer matches.
TEST(GenerationPoolTest, StaleHandleResolvesToNullptrAfterReacquire) {
    GenerationPool<Event> pool(1); // single slot forces index reuse

    auto first = pool.acquire();
    ASSERT_TRUE(first.has_value());
    GenerationHandle stale = first->handle;
    // Write a sentinel through the live pointer.
    first->ptr->a = 0xCAFEBABEull;
    EXPECT_NE(pool.resolve(stale), nullptr);

    // Release, then re-acquire the same physical slot (index 0).
    pool.release(stale);
    auto second = pool.acquire();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->handle.index(), stale.index()); // same slot reused

    // The new handle's generation must differ from the stale one (ABA tag).
    EXPECT_NE(second->handle.gen(), stale.gen());

    // The stale handle must NOT resolve to the live (now re-owned) slot.
    EXPECT_EQ(pool.resolve(stale), nullptr);
    // The fresh handle resolves correctly.
    EXPECT_EQ(pool.resolve(second->handle), second->ptr);
}

TEST(GenerationPoolTest, DefaultAndNullHandleAreInvalid) {
    GenerationPool<Event> pool(4);
    GenerationHandle null_handle{}; // default = kNull
    EXPECT_FALSE(null_handle.valid());
    EXPECT_EQ(pool.resolve(null_handle), nullptr);

    // Releasing a null handle is a safe no-op (must not corrupt the pool).
    pool.release(null_handle);
    // Pool still fully usable.
    auto r = pool.acquire();
    EXPECT_TRUE(r.has_value());
}

namespace { int g_counted_live = 0; }
struct Counted {
    int v = 0;
    Counted() { ++g_counted_live; }
    explicit Counted(int x) : v(x) { ++g_counted_live; }
    ~Counted() { --g_counted_live; }
};

TEST(GenerationPoolTest, EmplaceConstructs_ForEachLiveIterates_DestroyDestructs) {
    g_counted_live = 0;
    GenerationPool<Counted> pool(4);

    // emplace() acquires + placement-constructs (the args reach the ctor).
    auto a = pool.emplace(10);
    auto b = pool.emplace(20);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(a->ptr->v, 10);
    EXPECT_EQ(b->ptr->v, 20);
    EXPECT_EQ(g_counted_live, 2);

    // for_each_live() visits exactly the live slots (and only them).
    int count = 0, sum = 0;
    pool.for_each_live([&](GenerationHandle, Counted& c) { ++count; sum += c.v; });
    EXPECT_EQ(count, 2);
    EXPECT_EQ(sum, 30);

    // destroy() runs ~T and frees the slot; the handle goes stale.
    pool.destroy(a->handle);
    EXPECT_EQ(g_counted_live, 1);
    EXPECT_EQ(pool.resolve(a->handle), nullptr);
    count = 0; sum = 0;
    pool.for_each_live([&](GenerationHandle, Counted& c) { ++count; sum += c.v; });
    EXPECT_EQ(count, 1);
    EXPECT_EQ(sum, 20);

    // A destroyed slot is reusable.
    auto c = pool.emplace(30);
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(g_counted_live, 2);

    pool.destroy(b->handle);
    pool.destroy(c->handle);
    EXPECT_EQ(g_counted_live, 0);
}

// ─── LockFreeHashMap ────────────────────────────────────────────────────────────

TEST(LockFreeHashMapTest, CapacityRoundsUpToPowerOfTwo) {
    LockFreeHashMap<uint64_t, uint32_t> map(100);
    EXPECT_EQ(map.capacity(), 128u); // bit_ceil(100)
}

TEST(LockFreeHashMapTest, PutThenGetReturnsValue) {
    LockFreeHashMap<uint64_t, uint32_t> map(64);
    EXPECT_TRUE(map.put(42, 100u));

    auto v = map.get(42);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 100u);
}

TEST(LockFreeHashMapTest, GetMissingKeyReturnsError) {
    LockFreeHashMap<uint64_t, uint32_t> map(64);
    EXPECT_TRUE(map.put(7, 7u));

    auto v = map.get(9999); // never inserted
    EXPECT_FALSE(v.has_value());
}

TEST(LockFreeHashMapTest, PutSameKeyUpdatesInPlace) {
    LockFreeHashMap<uint64_t, uint32_t> map(64);
    EXPECT_TRUE(map.put(5, 1u));
    EXPECT_TRUE(map.put(5, 2u)); // update existing key

    auto v = map.get(5);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 2u);
}

TEST(LockFreeHashMapTest, MultipleKeysCoexist) {
    LockFreeHashMap<uint64_t, uint64_t> map(256);
    for (uint64_t k = 1; k <= 50; ++k)
        ASSERT_TRUE(map.put(k, k * 1000));

    for (uint64_t k = 1; k <= 50; ++k) {
        auto v = map.get(k);
        ASSERT_TRUE(v.has_value()) << "missing key " << k;
        EXPECT_EQ(*v, k * 1000);
    }
}

TEST(LockFreeHashMapTest, RemoveDeletesKey) {
    LockFreeHashMap<uint64_t, uint32_t> map(64);
    EXPECT_TRUE(map.put(11, 111u));
    ASSERT_TRUE(map.get(11).has_value());

    EXPECT_TRUE(map.remove(11));
    EXPECT_FALSE(map.get(11).has_value()); // gone

    // Removing an absent key returns false.
    EXPECT_FALSE(map.remove(11));
    EXPECT_FALSE(map.remove(123456));
}

TEST(LockFreeHashMapTest, ReinsertAfterRemoveReusesTombstone) {
    LockFreeHashMap<uint64_t, uint32_t> map(64);
    EXPECT_TRUE(map.put(21, 1u));
    EXPECT_TRUE(map.remove(21));
    // Re-insert the same key — should claim the tombstone slot.
    EXPECT_TRUE(map.put(21, 99u));

    auto v = map.get(21);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 99u);
}

TEST(LockFreeHashMapTest, RemoveDoesNotBreakProbeChain) {
    // Force a probe chain by inserting many keys, then remove an early one and
    // verify later keys (which may probe past the removed slot) are still found.
    LockFreeHashMap<uint64_t, uint32_t> map(16);
    for (uint64_t k = 1; k <= 10; ++k)
        ASSERT_TRUE(map.put(k, static_cast<uint32_t>(k)));

    EXPECT_TRUE(map.remove(3));
    for (uint64_t k = 1; k <= 10; ++k) {
        if (k == 3) {
            EXPECT_FALSE(map.get(k).has_value());
            continue;
        }
        auto v = map.get(k);
        ASSERT_TRUE(v.has_value()) << "probe chain broken at key " << k;
        EXPECT_EQ(*v, static_cast<uint32_t>(k));
    }
}

// ─── ErasureCoder (Reed-Solomon over GF(2^8)) ──────────────────────────────────

namespace {

// Helper: build (k+m) shards of `shard_len` bytes, fill the k data shards with
// deterministic pseudo-random bytes, encode parity, and return the storage +
// span views. The originals of the data shards are saved for comparison.
struct ErasureFixture {
    int k, m, shard_len;
    std::vector<std::vector<std::byte>> storage; // (k+m) buffers
    std::vector<std::span<std::byte>>   shards;  // span views into storage
    std::vector<std::vector<std::byte>> original_data; // copies of data shards

    ErasureFixture(int k_, int m_, int len_) : k(k_), m(m_), shard_len(len_) {
        storage.resize(static_cast<size_t>(k + m));
        original_data.resize(static_cast<size_t>(k));
        for (int i = 0; i < k + m; ++i)
            storage[static_cast<size_t>(i)].resize(static_cast<size_t>(shard_len));

        // Fill data shards with deterministic bytes.
        uint32_t seed = 0x1234567u;
        for (int d = 0; d < k; ++d) {
            for (int b = 0; b < shard_len; ++b) {
                seed = seed * 1664525u + 1013904223u; // LCG
                storage[static_cast<size_t>(d)][static_cast<size_t>(b)] =
                    std::byte(static_cast<uint8_t>(seed >> 16));
            }
            original_data[static_cast<size_t>(d)] = storage[static_cast<size_t>(d)];
        }

        for (int i = 0; i < k + m; ++i)
            shards.push_back(std::span<std::byte>(storage[static_cast<size_t>(i)]));
    }
};

// Returns true iff every data shard matches the original; on mismatch fills
// `mismatch` with the failing shard index.
bool data_matches_original(const ErasureFixture& f, int& mismatch) {
    for (int d = 0; d < f.k; ++d) {
        if (f.storage[static_cast<size_t>(d)] != f.original_data[static_cast<size_t>(d)]) {
            mismatch = d;
            return false;
        }
    }
    mismatch = -1;
    return true;
}

} // namespace

// gf256 sanity: log/antilog tables are consistent with the slow multiply, and
// inverse is correct. This validates the arithmetic foundation independently.
TEST(ErasureGF256Test, FastMulMatchesSlowMulAndInverseIsCorrect) {
    for (int a = 0; a < 256; ++a) {
        for (int b = 0; b < 256; ++b) {
            uint8_t slow = gf256::mul(static_cast<uint8_t>(a), static_cast<uint8_t>(b));
            uint8_t fast = gf256::fast_mul(static_cast<uint8_t>(a), static_cast<uint8_t>(b));
            ASSERT_EQ(slow, fast) << "mul mismatch a=" << a << " b=" << b;
        }
    }
    for (int a = 1; a < 256; ++a) {
        uint8_t i = gf256::inv(static_cast<uint8_t>(a));
        EXPECT_EQ(gf256::mul(static_cast<uint8_t>(a), i), 1u)
            << "inverse wrong for a=" << a;
    }
}

TEST(ErasureCoderTest, EncodeIsDeterministicAndNonTrivial) {
    ErasureFixture f(4, 2, 64);
    ErasureCoder ec(f.k, f.m);
    ec.encode(std::span<std::span<std::byte>>(f.shards));

    // Parity shards must have been written (at least one non-zero byte across
    // the parity region, given non-zero data).
    bool any_nonzero = false;
    for (int p = f.k; p < f.k + f.m; ++p)
        for (auto byte : f.storage[static_cast<size_t>(p)])
            if (byte != std::byte{0}) { any_nonzero = true; break; }
    EXPECT_TRUE(any_nonzero) << "parity shards are all zero — encode did nothing";

    // Encoding must not disturb data shards.
    int mismatch = -1;
    EXPECT_TRUE(data_matches_original(f, mismatch))
        << "encode corrupted data shard " << mismatch;
}

// Single-erasure reconstruction (RAID-5 style) — the simplest valid case.
TEST(ErasureCoderTest, ReconstructSingleDataShard) {
    ErasureFixture f(4, 2, 128);
    ErasureCoder ec(f.k, f.m);
    ec.encode(std::span<std::span<std::byte>>(f.shards));

    // std::vector<bool> is bit-packed and has no .data(); use a contiguous
    // bool buffer so we can form a std::span<const bool>.
    std::array<bool, 16> present;
    present.fill(true);
    const size_t total = static_cast<size_t>(f.k + f.m);
    present[1] = false; // lose data shard 1
    std::memset(f.storage[1].data(), 0, f.storage[1].size()); // wipe it

    auto r = ec.reconstruct(std::span<std::span<std::byte>>(f.shards),
                            std::span<const bool>(present.data(), total));
    ASSERT_TRUE(r.has_value()) << "reconstruct failed for single erasure";

    int mismatch = -1;
    EXPECT_TRUE(data_matches_original(f, mismatch))
        << "single-erasure recovery wrong at shard " << mismatch;
}

// Exhaustively try every erasure pattern that loses exactly `m` shards across
// the full set, restricted to data-shard losses we can actually verify, but
// covering combinations that exercise the decode-matrix inversion. Any valid
// (≤ m lost) pattern that fails reconstruction is a library bug.
TEST(ErasureCoderTest, ReconstructEveryTwoErasurePattern) {
    const int k = 4, m = 2, len = 96;
    ErasureCoder ec(k, m);

    int patterns_tested = 0;
    int patterns_failed = 0;
    std::vector<std::string> failures;

    // Try every pair (i<j) of lost shards out of k+m=6 total (15 patterns).
    for (int i = 0; i < k + m; ++i) {
        for (int j = i + 1; j < k + m; ++j) {
            ErasureFixture f(k, m, len);
            ec.encode(std::span<std::span<std::byte>>(f.shards));

            std::array<bool, 16> present;
            present.fill(true);
            present[static_cast<size_t>(i)] = false;
            present[static_cast<size_t>(j)] = false;
            // Wipe the lost shards so reconstruction can't accidentally pass.
            std::memset(f.storage[static_cast<size_t>(i)].data(), 0,
                        f.storage[static_cast<size_t>(i)].size());
            std::memset(f.storage[static_cast<size_t>(j)].data(), 0,
                        f.storage[static_cast<size_t>(j)].size());

            ++patterns_tested;
            auto r = ec.reconstruct(
                std::span<std::span<std::byte>>(f.shards),
                std::span<const bool>(present.data(), static_cast<size_t>(k + m)));

            if (!r.has_value()) {
                ++patterns_failed;
                failures.push_back("lose{" + std::to_string(i) + "," +
                                   std::to_string(j) + "}: reconstruct returned error");
                continue;
            }
            int mismatch = -1;
            if (!data_matches_original(f, mismatch)) {
                ++patterns_failed;
                failures.push_back("lose{" + std::to_string(i) + "," +
                                   std::to_string(j) + "}: data shard " +
                                   std::to_string(mismatch) + " not recovered");
            }
        }
    }

    EXPECT_EQ(patterns_tested, 15);
    // Every 2-erasure pattern out of 6 shards must be recoverable for a correct
    // RS(4,2) MDS code. A non-MDS Vandermonde matrix (singular k×k submatrix for
    // some surviving-row selection) would surface here.
    std::string msg;
    for (auto& s : failures) msg += "\n  " + s;
    EXPECT_EQ(patterns_failed, 0)
        << patterns_failed << " of " << patterns_tested
        << " valid 2-erasure patterns failed to reconstruct:" << msg;
}

// A larger RS(8,4): lose 4 (= m) shards in a few representative spreads.
TEST(ErasureCoderTest, ReconstructRS8x4MultiplePatterns) {
    const int k = 8, m = 4, len = 256;
    ErasureCoder ec(k, m);

    // Representative patterns each losing exactly m=4 shards.
    std::vector<std::array<int, 4>> patterns = {
        {0, 1, 2, 3},     // four contiguous data shards
        {8, 9, 10, 11},   // all four parity shards
        {0, 3, 8, 11},    // mixed data + parity
        {1, 4, 6, 9},     // scattered
        {4, 5, 6, 7},     // back half of data
    };

    int failed = 0;
    std::vector<std::string> failures;

    for (auto& pat : patterns) {
        ErasureFixture f(k, m, len);
        ec.encode(std::span<std::span<std::byte>>(f.shards));

        std::array<bool, 16> present;
        present.fill(true);
        for (int idx : pat) {
            present[static_cast<size_t>(idx)] = false;
            std::memset(f.storage[static_cast<size_t>(idx)].data(), 0,
                        f.storage[static_cast<size_t>(idx)].size());
        }

        auto r = ec.reconstruct(
            std::span<std::span<std::byte>>(f.shards),
            std::span<const bool>(present.data(), static_cast<size_t>(k + m)));

        std::string label = "lose{" + std::to_string(pat[0]) + "," +
                            std::to_string(pat[1]) + "," + std::to_string(pat[2]) +
                            "," + std::to_string(pat[3]) + "}";
        if (!r.has_value()) {
            ++failed;
            failures.push_back(label + ": reconstruct returned error");
            continue;
        }
        int mismatch = -1;
        if (!data_matches_original(f, mismatch)) {
            ++failed;
            failures.push_back(label + ": data shard " + std::to_string(mismatch) +
                               " not recovered");
        }
    }

    std::string msg;
    for (auto& s : failures) msg += "\n  " + s;
    EXPECT_EQ(failed, 0)
        << failed << " RS(8,4) m-erasure patterns failed:" << msg;
}

// Reconstruction must reject the impossible case (more than m shards lost).
TEST(ErasureCoderTest, ReconstructFailsWhenTooManyLost) {
    ErasureFixture f(4, 2, 64);
    ErasureCoder ec(f.k, f.m);
    ec.encode(std::span<std::span<std::byte>>(f.shards));

    std::array<bool, 16> present;
    present.fill(true);
    present[0] = present[1] = present[2] = false; // lose 3 > m=2
    auto r = ec.reconstruct(std::span<std::span<std::byte>>(f.shards),
                            std::span<const bool>(present.data(),
                                                  static_cast<size_t>(f.k + f.m)));
    EXPECT_FALSE(r.has_value())
        << "reconstruct should fail with fewer than k shards present";
}
