/**
 * @file tests/io_buffers_test.cpp
 * @brief Unit tests for the io/ buffer primitives: WriteBuf, BufferPool, IOVec<N>.
 *
 * Covers:
 *   WriteBuf
 *     - append (BufferView / string_view / raw ptr+len) accumulates contiguously
 *     - size / empty / clear (capacity preserved)
 *     - as_iovec() coalesces into a single iovec pointing at the whole buffer
 *     - exact coalesced byte content matches what was appended
 *   BufferPool<BufSize, Count>
 *     - acquire / return single-threaded round-trip
 *     - available() capacity accounting
 *     - exhaustion returns nullptr (no exception)
 *     - Buffer::release() returns to owning pool
 *     - distinct, cache-aligned, writable data regions
 *   IOVec<N>
 *     - push (raw ptr / BufferView / MutableBufferView)
 *     - empty() / full() at capacity / count
 *     - total_bytes()
 *     - as_scattered() round-trips through iov_data() / iov_count()
 *     - clear() resets
 *
 * All assertions check exact bytes / counts. No reactor / async involved, so
 * every test terminates deterministically.
 */

#include <gtest/gtest.h>

#include <qbuem/io/write_buf.hpp>
#include <qbuem/io/buffer_pool.hpp>
#include <qbuem/io/iovec.hpp>
#include <qbuem/io/scattered_span.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <string_view>
#include <vector>

using namespace qbuem;

// ─── Fixed test data ────────────────────────────────────────────────────────────

static const std::uint8_t kHdr[] = {0xDE, 0xAD};                  // 2 bytes
static const std::uint8_t kBody[] = {0x01, 0x02, 0x03, 0x04};     // 4 bytes
static const std::uint8_t kTail[] = {0xFF};                       // 1 byte

// ════════════════════════════════════════════════════════════════════════════════
// WriteBuf
// ════════════════════════════════════════════════════════════════════════════════

TEST(WriteBufTest, DefaultIsEmpty) {
    WriteBuf wb;
    EXPECT_TRUE(wb.empty());
    EXPECT_EQ(wb.size(), 0u);

    // Empty buffer → as_iovec() yields a zero-segment IOVec.
    auto vec = wb.as_iovec();
    EXPECT_EQ(vec.count, 0u);
    EXPECT_TRUE(vec.empty());
    EXPECT_EQ(vec.total_bytes(), 0u);
}

TEST(WriteBufTest, AppendBufferViewAccumulatesSize) {
    WriteBuf wb;
    wb.append(BufferView{kHdr, sizeof(kHdr)});
    EXPECT_EQ(wb.size(), 2u);
    wb.append(BufferView{kBody, sizeof(kBody)});
    EXPECT_EQ(wb.size(), 6u);
    EXPECT_FALSE(wb.empty());
}

TEST(WriteBufTest, AppendStringViewCopiesBytes) {
    WriteBuf wb;
    std::string_view sv = "hello";
    wb.append(sv);
    EXPECT_EQ(wb.size(), 5u);

    auto vec = wb.as_iovec();
    ASSERT_EQ(vec.count, 1u);
    const auto* p = static_cast<const std::byte*>(vec.vecs[0].iov_base);
    ASSERT_EQ(vec.vecs[0].iov_len, 5u);
    EXPECT_EQ(std::memcmp(p, sv.data(), sv.size()), 0);
}

TEST(WriteBufTest, AppendRawPtrLen) {
    WriteBuf wb;
    wb.append(kBody, sizeof(kBody));
    EXPECT_EQ(wb.size(), sizeof(kBody));

    auto vec = wb.as_iovec();
    ASSERT_EQ(vec.count, 1u);
    EXPECT_EQ(vec.vecs[0].iov_len, sizeof(kBody));
}

// Core behavior: multiple appends coalesce into ONE contiguous iovec segment,
// and the coalesced bytes are exactly hdr ++ body ++ tail.
TEST(WriteBufTest, MultiAppendCoalescesIntoSingleIovecWithExactBytes) {
    WriteBuf wb;
    wb.append(BufferView{kHdr, sizeof(kHdr)});
    wb.append(BufferView{kBody, sizeof(kBody)});
    wb.append(BufferView{kTail, sizeof(kTail)});

    const std::size_t expected_total = sizeof(kHdr) + sizeof(kBody) + sizeof(kTail);
    EXPECT_EQ(wb.size(), expected_total);  // 2 + 4 + 1 = 7

    auto vec = wb.as_iovec();
    // WriteBuf is a single contiguous buffer → always exactly one iovec entry.
    ASSERT_EQ(vec.count, 1u);
    EXPECT_EQ(vec.total_bytes(), expected_total);

    const auto* p = static_cast<const std::byte*>(vec.vecs[0].iov_base);
    ASSERT_EQ(vec.vecs[0].iov_len, expected_total);

    // Verify the exact coalesced byte sequence.
    std::array<std::uint8_t, 7> expected = {0xDE, 0xAD, 0x01, 0x02, 0x03, 0x04, 0xFF};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(std::to_integer<int>(p[i]), expected[i]) << "byte mismatch at index " << i;
    }
}

// The IOVec produced by as_iovec() must round-trip through scattered_span
// and deliver the same coalesced bytes when consumed as a segment view.
TEST(WriteBufTest, AsIovecRoundTripsThroughScatteredSpan) {
    WriteBuf wb;
    wb.append(BufferView{kHdr, sizeof(kHdr)});
    wb.append(BufferView{kBody, sizeof(kBody)});

    auto vec = wb.as_iovec();
    scattered_span s = vec.as_scattered();

    ASSERT_EQ(s.size(), 1u);
    EXPECT_EQ(s.iov_count(), 1);
    EXPECT_EQ(s.total_bytes(), 6u);

    auto seg = s[0];
    ASSERT_EQ(seg.size(), 6u);
    std::array<std::uint8_t, 6> expected = {0xDE, 0xAD, 0x01, 0x02, 0x03, 0x04};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(std::to_integer<int>(seg[i]), expected[i]) << "byte mismatch at index " << i;
    }
}

TEST(WriteBufTest, ClearResetsContentsButKeepsUsable) {
    WriteBuf wb;
    wb.append(BufferView{kBody, sizeof(kBody)});
    EXPECT_EQ(wb.size(), 4u);

    wb.clear();
    EXPECT_TRUE(wb.empty());
    EXPECT_EQ(wb.size(), 0u);
    EXPECT_EQ(wb.as_iovec().count, 0u);

    // Still usable after clear.
    wb.append(BufferView{kHdr, sizeof(kHdr)});
    EXPECT_EQ(wb.size(), 2u);
    auto vec = wb.as_iovec();
    ASSERT_EQ(vec.count, 1u);
    EXPECT_EQ(vec.vecs[0].iov_len, 2u);
    const auto* p = static_cast<const std::byte*>(vec.vecs[0].iov_base);
    EXPECT_EQ(std::to_integer<int>(p[0]), 0xDE);
    EXPECT_EQ(std::to_integer<int>(p[1]), 0xAD);
}

// ════════════════════════════════════════════════════════════════════════════════
// BufferPool
// ════════════════════════════════════════════════════════════════════════════════

TEST(BufferPoolTest, StartsFullyAvailable) {
    BufferPool<128, 4> pool;
    EXPECT_EQ(pool.available(), 4u);
}

TEST(BufferPoolTest, AcquireDecrementsAvailable) {
    BufferPool<64, 3> pool;
    auto* b0 = pool.acquire();
    ASSERT_NE(b0, nullptr);
    EXPECT_EQ(pool.available(), 2u);

    auto* b1 = pool.acquire();
    ASSERT_NE(b1, nullptr);
    EXPECT_EQ(pool.available(), 1u);

    // Acquired buffers are distinct.
    EXPECT_NE(b0, b1);

    b0->release();
    b1->release();
    EXPECT_EQ(pool.available(), 3u);
}

TEST(BufferPoolTest, ReturnBufferRestoresAvailability) {
    BufferPool<64, 2> pool;
    auto* b = pool.acquire();
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(pool.available(), 1u);

    pool.return_buffer(b);   // explicit return API
    EXPECT_EQ(pool.available(), 2u);
}

TEST(BufferPoolTest, ExhaustionReturnsNullptrNoException) {
    BufferPool<32, 2> pool;
    auto* b0 = pool.acquire();
    auto* b1 = pool.acquire();
    ASSERT_NE(b0, nullptr);
    ASSERT_NE(b1, nullptr);
    EXPECT_EQ(pool.available(), 0u);

    // Pool exhausted → nullptr, NOT an exception/throw.
    auto* b2 = pool.acquire();
    EXPECT_EQ(b2, nullptr);
    EXPECT_EQ(pool.available(), 0u);

    // Returning one makes a buffer available again.
    b0->release();
    EXPECT_EQ(pool.available(), 1u);
    auto* b3 = pool.acquire();
    EXPECT_NE(b3, nullptr);
    EXPECT_EQ(pool.available(), 0u);

    b1->release();
    b3->release();
}

TEST(BufferPoolTest, BufferDataIsWritableAndDistinct) {
    BufferPool<16, 3> pool;
    auto* b0 = pool.acquire();
    auto* b1 = pool.acquire();
    auto* b2 = pool.acquire();
    ASSERT_NE(b0, nullptr);
    ASSERT_NE(b1, nullptr);
    ASSERT_NE(b2, nullptr);

    // Distinct data regions (no aliasing).
    std::set<const std::byte*> regions{b0->data, b1->data, b2->data};
    EXPECT_EQ(regions.size(), 3u);

    // Each buffer's data is independently writable.
    std::memset(b0->data, 0xAA, 16);
    std::memset(b1->data, 0xBB, 16);
    std::memset(b2->data, 0xCC, 16);

    EXPECT_EQ(std::to_integer<int>(b0->data[0]), 0xAA);
    EXPECT_EQ(std::to_integer<int>(b1->data[7]), 0xBB);
    EXPECT_EQ(std::to_integer<int>(b2->data[15]), 0xCC);
    // No bleed between regions.
    EXPECT_EQ(std::to_integer<int>(b0->data[15]), 0xAA);

    b0->release();
    b1->release();
    b2->release();
}

TEST(BufferPoolTest, DataIsCacheLineAligned) {
    BufferPool<256, 2> pool;
    auto* b = pool.acquire();
    ASSERT_NE(b, nullptr);
    // Pillar 5 H4: shared mutable data is alignas(64).
    auto addr = reinterpret_cast<std::uintptr_t>(b->data);
    EXPECT_EQ(addr % 64u, 0u);
    b->release();
}

TEST(BufferPoolTest, ReleasedBufferOwnerPointerIsPool) {
    BufferPool<64, 1> pool;
    auto* b = pool.acquire();
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->pool, &pool);
    b->release();  // release() routes back to b->pool
    EXPECT_EQ(pool.available(), 1u);
}

TEST(BufferPoolTest, FullCycleConservesCount) {
    constexpr std::size_t N = 5;
    BufferPool<32, N> pool;

    std::vector<BufferPool<32, N>::Buffer*> held;
    // Drain the pool completely.
    for (std::size_t i = 0; i < N; ++i) {
        auto* b = pool.acquire();
        ASSERT_NE(b, nullptr) << "acquire #" << i << " unexpectedly null";
        held.push_back(b);
    }
    EXPECT_EQ(pool.available(), 0u);
    EXPECT_EQ(pool.acquire(), nullptr);

    // Return everything.
    for (auto* b : held) b->release();
    EXPECT_EQ(pool.available(), N);
}

// ════════════════════════════════════════════════════════════════════════════════
// IOVec<N>
// ════════════════════════════════════════════════════════════════════════════════

TEST(IOVecTest, DefaultIsEmpty) {
    IOVec<4> vec;
    EXPECT_EQ(vec.count, 0u);
    EXPECT_TRUE(vec.empty());
    EXPECT_FALSE(vec.full());
    EXPECT_EQ(vec.total_bytes(), 0u);
}

TEST(IOVecTest, PushRawPtrSetsEntry) {
    IOVec<4> vec;
    vec.push(kBody, sizeof(kBody));
    EXPECT_EQ(vec.count, 1u);
    EXPECT_FALSE(vec.empty());
    EXPECT_EQ(vec.vecs[0].iov_base, const_cast<void*>(static_cast<const void*>(kBody)));
    EXPECT_EQ(vec.vecs[0].iov_len, sizeof(kBody));
}

TEST(IOVecTest, PushBufferView) {
    IOVec<4> vec;
    vec.push(BufferView{kHdr, sizeof(kHdr)});
    EXPECT_EQ(vec.count, 1u);
    EXPECT_EQ(vec.vecs[0].iov_len, sizeof(kHdr));
    EXPECT_EQ(vec.vecs[0].iov_base, const_cast<void*>(static_cast<const void*>(kHdr)));
}

TEST(IOVecTest, PushMutableBufferView) {
    std::array<std::uint8_t, 3> scratch = {0x10, 0x20, 0x30};
    IOVec<4> vec;
    vec.push(MutableBufferView{scratch.data(), scratch.size()});
    EXPECT_EQ(vec.count, 1u);
    EXPECT_EQ(vec.vecs[0].iov_len, 3u);
    EXPECT_EQ(vec.vecs[0].iov_base, static_cast<void*>(scratch.data()));
}

TEST(IOVecTest, FullAtCapacity) {
    IOVec<2> vec;
    EXPECT_FALSE(vec.full());
    vec.push(kHdr, sizeof(kHdr));
    EXPECT_FALSE(vec.full());
    EXPECT_EQ(vec.count, 1u);
    vec.push(kBody, sizeof(kBody));
    EXPECT_TRUE(vec.full());
    EXPECT_EQ(vec.count, 2u);
    EXPECT_FALSE(vec.empty());
}

TEST(IOVecTest, TotalBytesSumsAllEntries) {
    IOVec<3> vec;
    vec.push(kHdr, sizeof(kHdr));    // 2
    vec.push(kBody, sizeof(kBody));  // 4
    vec.push(kTail, sizeof(kTail));  // 1
    EXPECT_EQ(vec.total_bytes(), 7u);
}

TEST(IOVecTest, ClearResetsCount) {
    IOVec<3> vec;
    vec.push(kHdr, sizeof(kHdr));
    vec.push(kBody, sizeof(kBody));
    EXPECT_EQ(vec.count, 2u);

    vec.clear();
    EXPECT_EQ(vec.count, 0u);
    EXPECT_TRUE(vec.empty());
    EXPECT_FALSE(vec.full());
    EXPECT_EQ(vec.total_bytes(), 0u);

    // Reusable after clear.
    vec.push(kTail, sizeof(kTail));
    EXPECT_EQ(vec.count, 1u);
    EXPECT_EQ(vec.total_bytes(), 1u);
}

TEST(IOVecTest, AsSpanReflectsValidEntries) {
    IOVec<8> vec;
    vec.push(kHdr, sizeof(kHdr));
    vec.push(kBody, sizeof(kBody));

    auto sp = vec.as_span();
    ASSERT_EQ(sp.size(), 2u);
    EXPECT_EQ(sp.data(), vec.vecs);
    EXPECT_EQ(sp[0].iov_len, sizeof(kHdr));
    EXPECT_EQ(sp[1].iov_len, sizeof(kBody));

    auto csp = vec.as_const_span();
    EXPECT_EQ(csp.size(), 2u);
    EXPECT_EQ(csp.data(), vec.vecs);
}

// as_scattered() must round-trip exactly through iov_data() / iov_count():
// same pointer, same count, same per-segment bytes.
TEST(IOVecTest, AsScatteredRoundTripsThroughIovDataAndCount) {
    IOVec<3> vec;
    vec.push(kHdr, sizeof(kHdr));
    vec.push(kBody, sizeof(kBody));
    vec.push(kTail, sizeof(kTail));

    scattered_span s = vec.as_scattered();

    // iov_data() points at the backing IOVec array; iov_count() matches count.
    EXPECT_EQ(s.iov_data(), vec.vecs);
    EXPECT_EQ(s.iov_count(), static_cast<int>(vec.count));
    EXPECT_EQ(s.size(), vec.count);
    EXPECT_EQ(s.total_bytes(), vec.total_bytes());

    // Reconstruct each segment from iov_data() / iov_count() and compare bytes.
    const iovec* iov = s.iov_data();
    const int n = s.iov_count();
    ASSERT_EQ(n, 3);

    // Segment 0 == kHdr
    ASSERT_EQ(iov[0].iov_len, sizeof(kHdr));
    EXPECT_EQ(std::memcmp(iov[0].iov_base, kHdr, sizeof(kHdr)), 0);
    // Segment 1 == kBody
    ASSERT_EQ(iov[1].iov_len, sizeof(kBody));
    EXPECT_EQ(std::memcmp(iov[1].iov_base, kBody, sizeof(kBody)), 0);
    // Segment 2 == kTail
    ASSERT_EQ(iov[2].iov_len, sizeof(kTail));
    EXPECT_EQ(std::memcmp(iov[2].iov_base, kTail, sizeof(kTail)), 0);

    // And via the scattered_span segment view.
    EXPECT_EQ(std::to_integer<int>(s[0][0]), 0xDE);
    EXPECT_EQ(std::to_integer<int>(s[0][1]), 0xAD);
    EXPECT_EQ(std::to_integer<int>(s[1][0]), 0x01);
    EXPECT_EQ(std::to_integer<int>(s[1][3]), 0x04);
    EXPECT_EQ(std::to_integer<int>(s[2][0]), 0xFF);
}

TEST(IOVecTest, EmptyIovecScattersToEmpty) {
    IOVec<4> vec;
    scattered_span s = vec.as_scattered();
    EXPECT_EQ(s.iov_count(), 0);
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.total_bytes(), 0u);
}
