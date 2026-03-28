/**
 * @file tests/scattered_span_test.cpp
 * @brief Unit tests for scattered_span, IOVec<N>::as_scattered(), and make_scattered_span.
 *
 * Covers:
 *   - Construction (from IOVec<N>, span<const iovec>, span<iovec>)
 *   - POSIX syscall accessors (iov_data, iov_count, as_iovec)
 *   - Implicit conversion to span<const iovec>
 *   - Size / empty / total_bytes
 *   - Element access (operator[], front, back)
 *   - Range iteration
 *   - subspan
 *   - make_scattered_span factory
 *   - IOVec<N>::as_scattered() shorthand
 *   - Multi-segment total_bytes correctness
 *   - Pointer / data integrity (no copy)
 */

#include <gtest/gtest.h>
#include <qbuem/io/scattered_span.hpp>
#include <qbuem/io/iovec.hpp>

#include <array>
#include <cstring>
#include <span>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

using namespace qbuem;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static const unsigned char kA[] = {0x01, 0x02, 0x03};
static const unsigned char kB[] = {0x04, 0x05};
static const unsigned char kC[] = {0x06};

// ─── Construction ─────────────────────────────────────────────────────────────

TEST(ScatteredSpanTest, DefaultConstructedIsEmpty) {
    scattered_span s;
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.size(), 0u);
    EXPECT_EQ(s.total_bytes(), 0u);
    EXPECT_EQ(s.iov_count(), 0);
    EXPECT_EQ(s.iov_data(), nullptr);
}

TEST(ScatteredSpanTest, ConstructFromIOVec) {
    IOVec<4> vec;
    vec.push(kA, sizeof(kA));
    vec.push(kB, sizeof(kB));

    scattered_span s{vec};
    EXPECT_EQ(s.size(), 2u);
    EXPECT_FALSE(s.empty());
}

TEST(ScatteredSpanTest, ConstructFromConstSpanIovec) {
    IOVec<2> vec;
    vec.push(kA, sizeof(kA));

    std::span<const iovec> sv = vec.as_const_span();
    scattered_span s{sv};
    EXPECT_EQ(s.size(), 1u);
}

TEST(ScatteredSpanTest, ConstructFromMutableSpanIovec) {
    IOVec<2> vec;
    vec.push(kA, sizeof(kA));

    std::span<iovec> sv = vec.as_span();
    scattered_span s{sv};
    EXPECT_EQ(s.size(), 1u);
}

TEST(ScatteredSpanTest, AsScatteredShorthand) {
    IOVec<3> vec;
    vec.push(kA, sizeof(kA));
    vec.push(kB, sizeof(kB));
    vec.push(kC, sizeof(kC));

    auto s = vec.as_scattered();
    EXPECT_EQ(s.size(), 3u);
}

// ─── POSIX syscall accessors ──────────────────────────────────────────────────

TEST(ScatteredSpanTest, IovDataPointsToFirstIovec) {
    IOVec<2> vec;
    vec.push(kA, sizeof(kA));
    vec.push(kB, sizeof(kB));
    scattered_span s{vec};

    EXPECT_EQ(s.iov_data(), vec.vecs);
    EXPECT_EQ(s.iov_count(), 2);
}

TEST(ScatteredSpanTest, AsIovecMatchesUnderlying) {
    IOVec<2> vec;
    vec.push(kA, sizeof(kA));
    vec.push(kB, sizeof(kB));
    scattered_span s{vec};

    auto sv = s.as_iovec();
    EXPECT_EQ(sv.size(), 2u);
    EXPECT_EQ(sv.data(), vec.vecs);
}

TEST(ScatteredSpanTest, ImplicitConversionToSpan) {
    IOVec<2> vec;
    vec.push(kA, sizeof(kA));
    scattered_span s{vec};

    std::span<const iovec> sv = s;  // implicit conversion
    EXPECT_EQ(sv.size(), 1u);
    EXPECT_EQ(sv.data(), vec.vecs);
}

// ─── Size / total_bytes ───────────────────────────────────────────────────────

TEST(ScatteredSpanTest, TotalBytesIsSegmentSum) {
    IOVec<3> vec;
    vec.push(kA, sizeof(kA));  // 3
    vec.push(kB, sizeof(kB));  // 2
    vec.push(kC, sizeof(kC));  // 1
    scattered_span s{vec};

    EXPECT_EQ(s.total_bytes(), 6u);
}

TEST(ScatteredSpanTest, TotalBytesEmptyIsZero) {
    IOVec<4> vec;
    scattered_span s{vec};
    EXPECT_EQ(s.total_bytes(), 0u);
}

// ─── Element access ───────────────────────────────────────────────────────────

TEST(ScatteredSpanTest, OperatorIndexReturnsCorrectSegment) {
    IOVec<2> vec;
    vec.push(kA, sizeof(kA));
    vec.push(kB, sizeof(kB));
    scattered_span s{vec};

    auto seg0 = s[0];
    EXPECT_EQ(seg0.size(), sizeof(kA));
    EXPECT_EQ(seg0.data(), reinterpret_cast<const std::byte*>(kA));

    auto seg1 = s[1];
    EXPECT_EQ(seg1.size(), sizeof(kB));
    EXPECT_EQ(seg1.data(), reinterpret_cast<const std::byte*>(kB));
}

TEST(ScatteredSpanTest, FrontReturnsFirstSegment) {
    IOVec<2> vec;
    vec.push(kA, sizeof(kA));
    vec.push(kB, sizeof(kB));
    scattered_span s{vec};

    EXPECT_EQ(s.front().size(), sizeof(kA));
    EXPECT_EQ(s.front().data(), reinterpret_cast<const std::byte*>(kA));
}

TEST(ScatteredSpanTest, BackReturnsLastSegment) {
    IOVec<2> vec;
    vec.push(kA, sizeof(kA));
    vec.push(kB, sizeof(kB));
    scattered_span s{vec};

    EXPECT_EQ(s.back().size(), sizeof(kB));
    EXPECT_EQ(s.back().data(), reinterpret_cast<const std::byte*>(kB));
}

TEST(ScatteredSpanTest, SingleSegmentFrontEqualsBack) {
    IOVec<1> vec;
    vec.push(kA, sizeof(kA));
    scattered_span s{vec};

    EXPECT_EQ(s.front().data(), s.back().data());
}

// ─── Pointer integrity (zero-copy guarantee) ──────────────────────────────────

TEST(ScatteredSpanTest, SegmentPointersAreOriginalBuffers) {
    // No copies — iov_base must equal the original pointer.
    IOVec<3> vec;
    vec.push(kA, sizeof(kA));
    vec.push(kB, sizeof(kB));
    vec.push(kC, sizeof(kC));
    scattered_span s{vec};

    EXPECT_EQ(s[0].data(), reinterpret_cast<const std::byte*>(kA));
    EXPECT_EQ(s[1].data(), reinterpret_cast<const std::byte*>(kB));
    EXPECT_EQ(s[2].data(), reinterpret_cast<const std::byte*>(kC));
}

TEST(ScatteredSpanTest, SegmentContentsMatchOriginalData) {
    IOVec<2> vec;
    vec.push(kA, sizeof(kA));
    vec.push(kB, sizeof(kB));
    scattered_span s{vec};

    auto seg0 = s[0];
    EXPECT_EQ(std::to_integer<int>(seg0[0]), 0x01);
    EXPECT_EQ(std::to_integer<int>(seg0[1]), 0x02);
    EXPECT_EQ(std::to_integer<int>(seg0[2]), 0x03);

    auto seg1 = s[1];
    EXPECT_EQ(std::to_integer<int>(seg1[0]), 0x04);
    EXPECT_EQ(std::to_integer<int>(seg1[1]), 0x05);
}

// ─── Range iteration ──────────────────────────────────────────────────────────

TEST(ScatteredSpanTest, RangeForIteratesAllSegments) {
    IOVec<3> vec;
    vec.push(kA, sizeof(kA));
    vec.push(kB, sizeof(kB));
    vec.push(kC, sizeof(kC));
    scattered_span s{vec};

    std::size_t count = 0;
    std::size_t total = 0;
    for (auto seg : s) {
        total += seg.size();
        ++count;
    }
    EXPECT_EQ(count, 3u);
    EXPECT_EQ(total, 6u);
}

TEST(ScatteredSpanTest, BeginEndIteratorArithmetic) {
    IOVec<3> vec;
    vec.push(kA, sizeof(kA));
    vec.push(kB, sizeof(kB));
    vec.push(kC, sizeof(kC));
    scattered_span s{vec};

    auto it = s.begin();
    EXPECT_EQ((*it).size(), sizeof(kA));
    ++it;
    EXPECT_EQ((*it).size(), sizeof(kB));
    it++;
    EXPECT_EQ((*it).size(), sizeof(kC));
    ++it;
    EXPECT_EQ(it, s.end());
}

TEST(ScatteredSpanTest, IteratorDistanceEqualsSize) {
    IOVec<4> vec;
    vec.push(kA, sizeof(kA));
    vec.push(kB, sizeof(kB));
    scattered_span s{vec};

    EXPECT_EQ(s.end() - s.begin(), static_cast<std::ptrdiff_t>(s.size()));
}

TEST(ScatteredSpanTest, CbeginCendWork) {
    IOVec<2> vec;
    vec.push(kA, sizeof(kA));
    scattered_span s{vec};

    EXPECT_EQ(s.cbegin(), s.begin());
    EXPECT_EQ(s.cend(),   s.end());
}

// ─── subspan ──────────────────────────────────────────────────────────────────

TEST(ScatteredSpanTest, SubspanFromOffset) {
    IOVec<3> vec;
    vec.push(kA, sizeof(kA));
    vec.push(kB, sizeof(kB));
    vec.push(kC, sizeof(kC));
    scattered_span s{vec};

    auto sub = s.subspan(1);
    EXPECT_EQ(sub.size(), 2u);
    EXPECT_EQ(sub[0].data(), reinterpret_cast<const std::byte*>(kB));
    EXPECT_EQ(sub[1].data(), reinterpret_cast<const std::byte*>(kC));
}

TEST(ScatteredSpanTest, SubspanWithCount) {
    IOVec<3> vec;
    vec.push(kA, sizeof(kA));
    vec.push(kB, sizeof(kB));
    vec.push(kC, sizeof(kC));
    scattered_span s{vec};

    auto sub = s.subspan(0, 2);
    EXPECT_EQ(sub.size(), 2u);
    EXPECT_EQ(sub[0].data(), reinterpret_cast<const std::byte*>(kA));
    EXPECT_EQ(sub[1].data(), reinterpret_cast<const std::byte*>(kB));
}

TEST(ScatteredSpanTest, SubspanAllIsIdentity) {
    IOVec<2> vec;
    vec.push(kA, sizeof(kA));
    vec.push(kB, sizeof(kB));
    scattered_span s{vec};

    auto sub = s.subspan(0);
    EXPECT_EQ(sub.size(), s.size());
    EXPECT_EQ(sub.iov_data(), s.iov_data());
}

TEST(ScatteredSpanTest, SubspanEmptyWhenOffsetEqualsSize) {
    IOVec<2> vec;
    vec.push(kA, sizeof(kA));
    scattered_span s{vec};

    auto sub = s.subspan(1);
    EXPECT_TRUE(sub.empty());
}

// ─── make_scattered_span ──────────────────────────────────────────────────────

TEST(ScatteredSpanTest, MakeScatteredSpanSingleView) {
    IOVec<4> storage;
    BufferView bv{kA, sizeof(kA)};
    auto s = make_scattered_span(storage, bv);

    EXPECT_EQ(s.size(), 1u);
    EXPECT_EQ(s.total_bytes(), sizeof(kA));
    EXPECT_EQ(s[0].data(), reinterpret_cast<const std::byte*>(kA));
}

TEST(ScatteredSpanTest, MakeScatteredSpanMultipleViews) {
    IOVec<4> storage;
    BufferView b0{kA, sizeof(kA)};
    BufferView b1{kB, sizeof(kB)};
    BufferView b2{kC, sizeof(kC)};
    auto s = make_scattered_span(storage, b0, b1, b2);

    EXPECT_EQ(s.size(), 3u);
    EXPECT_EQ(s.total_bytes(), 6u);
    EXPECT_EQ(s[0].data(), reinterpret_cast<const std::byte*>(kA));
    EXPECT_EQ(s[1].data(), reinterpret_cast<const std::byte*>(kB));
    EXPECT_EQ(s[2].data(), reinterpret_cast<const std::byte*>(kC));
}

TEST(ScatteredSpanTest, MakeScatteredSpanClearsStorage) {
    IOVec<4> storage;
    storage.push(kA, sizeof(kA));
    storage.push(kA, sizeof(kA));  // pre-populated

    BufferView b{kB, sizeof(kB)};
    auto s = make_scattered_span(storage, b);

    // storage.clear() is called internally — only the new segment should appear
    EXPECT_EQ(s.size(), 1u);
    EXPECT_EQ(storage.count, 1u);
}

// ─── IOVec capacity / empty / full interaction ────────────────────────────────

TEST(ScatteredSpanTest, EmptyIOVecProducesEmptyScatter) {
    IOVec<8> vec;
    // No push — vec.count == 0
    scattered_span s{vec};
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.total_bytes(), 0u);
}

TEST(ScatteredSpanTest, FullIOVecAllSegmentsReflected) {
    IOVec<3> vec;
    vec.push(kA, sizeof(kA));
    vec.push(kB, sizeof(kB));
    vec.push(kC, sizeof(kC));
    EXPECT_TRUE(vec.full());

    scattered_span s = vec.as_scattered();
    EXPECT_EQ(s.size(), 3u);
    EXPECT_EQ(s.total_bytes(), sizeof(kA) + sizeof(kB) + sizeof(kC));
}

// ─── Syscall-readiness pattern ────────────────────────────────────────────────

TEST(ScatteredSpanTest, WritevPatternCompiles) {
    // Verify the intended writev(2) usage pattern compiles and runs without UB.
    // We write to STDOUT_FILENO (fd=1) if it is a pipe/tty — but to keep the
    // test hermetic we use a socketpair instead.
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    IOVec<2> vec;
    vec.push(kA, sizeof(kA));
    vec.push(kB, sizeof(kB));
    scattered_span s{vec};

    ssize_t n = ::writev(sv[1], s.iov_data(), s.iov_count());
    EXPECT_EQ(n, static_cast<ssize_t>(s.total_bytes()));

    std::array<unsigned char, 8> buf{};
    ssize_t r = ::read(sv[0], buf.data(), buf.size());
    EXPECT_EQ(r, 5);
    EXPECT_EQ(buf[0], kA[0]);
    EXPECT_EQ(buf[3], kB[0]);

    ::close(sv[0]);
    ::close(sv[1]);
}
