/**
 * @file tests/io_buf_test.cpp
 * @brief Unit tests for IOSlice, MutableIOSlice, ReadBuf<N>, WriteBuf, IOVec<N>.
 */

#include <gtest/gtest.h>
#include <qbuem/io/io_slice.hpp>
#include <qbuem/io/iovec.hpp>
#include <qbuem/io/read_buf.hpp>
#include <qbuem/io/write_buf.hpp>

#include <cstring>
#include <string_view>

using namespace qbuem;

// ─── IOSlice ──────────────────────────────────────────────────────────────────

TEST(IOSliceTest, ToBufferViewReturnsCorrectSpan) {
    std::array<std::byte, 8> data{};
    data[0] = std::byte{0xAB};
    IOSlice slice{data.data(), data.size()};

    auto bv = slice.to_buffer_view();
    EXPECT_EQ(bv.size(), 8u);
    EXPECT_EQ(bv[0], 0xABu);
}

TEST(IOSliceTest, ToIovecHasCorrectLen) {
    std::array<std::byte, 16> data{};
    IOSlice slice{data.data(), data.size()};

    auto iov = slice.to_iovec();
    EXPECT_EQ(iov.iov_len, 16u);
    EXPECT_NE(iov.iov_base, nullptr);
}

// ─── MutableIOSlice ───────────────────────────────────────────────────────────

TEST(MutableIOSliceTest, ToBufferViewIsMutable) {
    std::array<std::byte, 8> data{};
    MutableIOSlice slice{data.data(), data.size()};

    auto bv = slice.to_buffer_view();
    EXPECT_EQ(bv.size(), 8u);
    bv[0] = 0xCDu;
    EXPECT_EQ(data[0], std::byte{0xCDu});
}

TEST(MutableIOSliceTest, AsConstConvertsToReadOnly) {
    std::array<std::byte, 4> data{};
    MutableIOSlice slice{data.data(), data.size()};

    IOSlice ro = slice.as_const();
    EXPECT_EQ(ro.size, 4u);
    EXPECT_EQ(ro.data, data.data());
}

TEST(MutableIOSliceTest, ToIovecBaseIsWritable) {
    std::array<std::byte, 4> data{};
    MutableIOSlice slice{data.data(), data.size()};

    auto iov = slice.to_iovec();
    // iov_base can be written to
    std::memset(iov.iov_base, 0xFF, iov.iov_len);
    EXPECT_EQ(data[0], std::byte{0xFFu});
}

// ─── ReadBuf<N> ───────────────────────────────────────────────────────────────

TEST(ReadBufTest, InitialStateIsEmpty) {
    ReadBuf<1024> buf;
    EXPECT_EQ(buf.size(), 0u);
    EXPECT_TRUE(buf.empty());
    EXPECT_FALSE(buf.full());
    EXPECT_EQ(buf.writable_size(), 1024u);
}

TEST(ReadBufTest, CommitIncreasesSize) {
    ReadBuf<256> buf;
    // Simulate writing 10 bytes
    std::memset(buf.write_head(), 0xAA, 10);
    buf.commit(10);
    EXPECT_EQ(buf.size(), 10u);
    EXPECT_EQ(buf.writable_size(), 246u);
}

TEST(ReadBufTest, ReadableViewMatchesCommitted) {
    ReadBuf<256> buf;
    const char msg[] = "hello";
    std::memcpy(buf.write_head(), msg, 5);
    buf.commit(5);

    auto view = buf.readable();
    EXPECT_EQ(view.size(), 5u);
    EXPECT_EQ(std::memcmp(view.data(), msg, 5), 0);
}

TEST(ReadBufTest, ConsumeReducesSize) {
    ReadBuf<256> buf;
    std::memset(buf.write_head(), 0x00, 20);
    buf.commit(20);
    buf.consume(8);
    EXPECT_EQ(buf.size(), 12u);
}

TEST(ReadBufTest, CompactMakesWriteSpaceAvailable) {
    ReadBuf<16> buf;
    // Fill the buffer to the end
    std::memset(buf.write_head(), 0x01, 16);
    buf.commit(16);
    // Consume some to create a gap
    buf.consume(8);

    // write_pos == 16, read_pos == 8 → compact moves 8 bytes
    buf.compact();
    EXPECT_EQ(buf.size(), 8u);
    EXPECT_EQ(buf.writable_size(), 8u);
}

TEST(ReadBufTest, ResetClearsBuffer) {
    ReadBuf<128> buf;
    std::memset(buf.write_head(), 0x01, 10);
    buf.commit(10);
    buf.reset();
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(buf.writable_size(), 128u);
}

TEST(ReadBufTest, FullWhenNoWritableSpace) {
    ReadBuf<4> buf;
    std::memset(buf.write_head(), 0x00, 4);
    buf.commit(4);
    EXPECT_TRUE(buf.full());
    EXPECT_EQ(buf.writable_size(), 0u);
}

// ─── WriteBuf ─────────────────────────────────────────────────────────────────

TEST(WriteBufTest, InitiallyEmpty) {
    WriteBuf wb;
    EXPECT_TRUE(wb.empty());
    EXPECT_EQ(wb.size(), 0u);
}

TEST(WriteBufTest, AppendStringView) {
    WriteBuf wb;
    wb.append("hello");
    EXPECT_EQ(wb.size(), 5u);
    EXPECT_FALSE(wb.empty());
}

TEST(WriteBufTest, AppendMultiple) {
    WriteBuf wb;
    wb.append("GET ");
    wb.append("/ ");
    wb.append("HTTP/1.1\r\n");
    EXPECT_EQ(wb.size(), 16u);
}

TEST(WriteBufTest, AsIovecSingleEntry) {
    WriteBuf wb;
    wb.append("data");
    auto vec = wb.as_iovec();
    EXPECT_EQ(vec.count, 1u);
    EXPECT_EQ(vec.vecs[0].iov_len, 4u);
}

TEST(WriteBufTest, EmptyBufGivesEmptyIovec) {
    WriteBuf wb;
    auto vec = wb.as_iovec();
    EXPECT_EQ(vec.count, 0u);
}

TEST(WriteBufTest, ClearResetsSize) {
    WriteBuf wb;
    wb.append("some data");
    wb.clear();
    EXPECT_TRUE(wb.empty());
    EXPECT_EQ(wb.size(), 0u);
}

TEST(WriteBufTest, AppendBufferView) {
    WriteBuf wb;
    const uint8_t data[] = {1, 2, 3, 4};
    BufferView bv{data, 4};
    wb.append(bv);
    EXPECT_EQ(wb.size(), 4u);
}

// ─── IOVec<N> ─────────────────────────────────────────────────────────────────

TEST(IOVecTest, PushAndCount) {
    IOVec<4> vec;
    EXPECT_TRUE(vec.empty());
    EXPECT_EQ(vec.count, 0u);

    const char a[] = "hello";
    vec.push(a, 5);
    EXPECT_EQ(vec.count, 1u);
    EXPECT_FALSE(vec.empty());
}

TEST(IOVecTest, TotalBytesCalculation) {
    IOVec<4> vec;
    const char a[] = "hello";
    const char b[] = "world!!";
    vec.push(a, 5);
    vec.push(b, 7);
    EXPECT_EQ(vec.total_bytes(), 12u);
}

TEST(IOVecTest, PushBufferView) {
    IOVec<4> vec;
    const uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    BufferView bv{data, 4};
    vec.push(bv);
    EXPECT_EQ(vec.count, 1u);
    EXPECT_EQ(vec.vecs[0].iov_len, 4u);
}

TEST(IOVecTest, ClearResetsCount) {
    IOVec<4> vec;
    const char a[] = "x";
    vec.push(a, 1);
    vec.push(a, 1);
    vec.clear();
    EXPECT_TRUE(vec.empty());
    EXPECT_EQ(vec.count, 0u);
}

TEST(IOVecTest, FullDetection) {
    IOVec<2> vec;
    const char a[] = "x";
    vec.push(a, 1);
    EXPECT_FALSE(vec.full());
    vec.push(a, 1);
    EXPECT_TRUE(vec.full());
}

TEST(IOVecTest, AsSpanSize) {
    IOVec<8> vec;
    const char a[] = "abc";
    vec.push(a, 3);
    vec.push(a, 3);
    EXPECT_EQ(vec.as_span().size(), 2u);
    EXPECT_EQ(vec.as_const_span().size(), 2u);
}
