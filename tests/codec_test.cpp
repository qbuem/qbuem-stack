/**
 * @file tests/codec_test.cpp
 * @brief Unit tests for LengthPrefixedCodec and LineCodec.
 */

#include <gtest/gtest.h>
#include <qbuem/codec/frame_codec.hpp>
#include <qbuem/codec/length_prefix_codec.hpp>
#include <qbuem/codec/line_codec.hpp>

#include <cstring>
#include <string>
#include <vector>

using namespace qbuem::codec;

// ─── LengthPrefixedCodec ──────────────────────────────────────────────────────

TEST(LengthPrefixedCodecTest, EncodeProducesTwo_Iovecs) {
    LengthPrefixedCodec codec;
    LengthPrefixedFrame frame;
    const char payload[] = "hello";
    frame.length = 5;
    frame.payload.assign(reinterpret_cast<const std::byte*>(payload),
                         reinterpret_cast<const std::byte*>(payload) + 5);

    iovec vecs[2];
    size_t n = codec.encode(frame, vecs, 2, nullptr);
    EXPECT_EQ(n, 2u);
    EXPECT_EQ(vecs[0].iov_len, 4u);  // 4-byte header
    EXPECT_EQ(vecs[1].iov_len, 5u);  // payload
}

TEST(LengthPrefixedCodecTest, EncodeHeaderIsBigEndian) {
    LengthPrefixedCodec codec;
    LengthPrefixedFrame frame;
    frame.length = 0x00000005u;
    frame.payload.resize(5);

    iovec vecs[2];
    codec.encode(frame, vecs, 2, nullptr);

    const auto* hdr = static_cast<const uint8_t*>(vecs[0].iov_base);
    EXPECT_EQ(hdr[0], 0x00);
    EXPECT_EQ(hdr[1], 0x00);
    EXPECT_EQ(hdr[2], 0x00);
    EXPECT_EQ(hdr[3], 0x05);
}

TEST(LengthPrefixedCodecTest, EncodeThenDecodeRoundTrip) {
    LengthPrefixedCodec enc_codec;
    LengthPrefixedCodec dec_codec;

    const std::string payload_str = "ping-pong";
    LengthPrefixedFrame send_frame;
    send_frame.length = static_cast<uint32_t>(payload_str.size());
    send_frame.payload.assign(
        reinterpret_cast<const std::byte*>(payload_str.data()),
        reinterpret_cast<const std::byte*>(payload_str.data()) + payload_str.size());

    // Encode
    iovec vecs[2];
    size_t n = enc_codec.encode(send_frame, vecs, 2, nullptr);
    ASSERT_EQ(n, 2u);

    // Build wire bytes: header + payload
    std::vector<uint8_t> wire;
    wire.insert(wire.end(),
                static_cast<const uint8_t*>(vecs[0].iov_base),
                static_cast<const uint8_t*>(vecs[0].iov_base) + vecs[0].iov_len);
    wire.insert(wire.end(),
                static_cast<const uint8_t*>(vecs[1].iov_base),
                static_cast<const uint8_t*>(vecs[1].iov_base) + vecs[1].iov_len);

    // Decode
    BufferView buf{wire.data(), wire.size()};
    LengthPrefixedFrame recv_frame;
    auto status = dec_codec.decode(buf, recv_frame);

    EXPECT_EQ(status, DecodeStatus::Complete);
    EXPECT_EQ(recv_frame.length, send_frame.length);
    EXPECT_EQ(recv_frame.payload.size(), payload_str.size());
    EXPECT_EQ(std::memcmp(recv_frame.payload.data(), payload_str.data(), payload_str.size()), 0);
}

TEST(LengthPrefixedCodecTest, IncompleteHeader) {
    LengthPrefixedCodec codec;
    LengthPrefixedFrame frame;

    // Only 2 bytes — header is incomplete (needs 4)
    std::vector<uint8_t> partial = {0x00, 0x00};
    BufferView buf{partial.data(), partial.size()};
    auto status = codec.decode(buf, frame);
    EXPECT_EQ(status, DecodeStatus::Incomplete);
}

TEST(LengthPrefixedCodecTest, IncompleteBody) {
    LengthPrefixedCodec codec;
    LengthPrefixedFrame frame;

    // 4-byte header saying length=10, but only 3 payload bytes provided
    std::vector<uint8_t> wire = {0x00, 0x00, 0x00, 0x0A, 0x01, 0x02, 0x03};
    BufferView buf{wire.data(), wire.size()};
    auto status = codec.decode(buf, frame);
    EXPECT_EQ(status, DecodeStatus::Incomplete);
}

TEST(LengthPrefixedCodecTest, ResetClearsState) {
    LengthPrefixedCodec codec;
    LengthPrefixedFrame frame;

    // Partial data — then reset
    std::vector<uint8_t> partial = {0x00, 0x00};
    BufferView buf{partial.data(), partial.size()};
    codec.decode(buf, frame);
    codec.reset();

    // After reset, can decode again from scratch
    std::vector<uint8_t> wire = {0x00, 0x00, 0x00, 0x02, 0xAB, 0xCD};
    BufferView buf2{wire.data(), wire.size()};
    auto status = codec.decode(buf2, frame);
    EXPECT_EQ(status, DecodeStatus::Complete);
    EXPECT_EQ(frame.length, 2u);
}

TEST(LengthPrefixedCodecTest, EncodeRequiresAtLeastTwoVecs) {
    LengthPrefixedCodec codec;
    LengthPrefixedFrame frame;
    frame.length = 1;
    frame.payload.resize(1);

    iovec vecs[2];
    EXPECT_EQ(codec.encode(frame, vecs, 1, nullptr), 0u);  // < 2 vecs → 0
    EXPECT_EQ(codec.encode(frame, vecs, 2, nullptr), 2u);
}

// ─── LineCodec ────────────────────────────────────────────────────────────────

TEST(LineCodecTest, CrlfLineDecoded) {
    LineCodec codec(/*crlf=*/true);
    Line line;
    const std::string data = "GET / HTTP/1.1\r\n";
    BufferView buf{reinterpret_cast<const uint8_t*>(data.data()), data.size()};
    auto status = codec.decode(buf, line);
    EXPECT_EQ(status, DecodeStatus::Complete);
    EXPECT_EQ(line.data, "GET / HTTP/1.1");
}

TEST(LineCodecTest, LfOnlyLine) {
    LineCodec codec(/*crlf=*/false);
    Line line;
    const std::string data = "hello\n";
    BufferView buf{reinterpret_cast<const uint8_t*>(data.data()), data.size()};
    auto status = codec.decode(buf, line);
    EXPECT_EQ(status, DecodeStatus::Complete);
    EXPECT_EQ(line.data, "hello");
}

TEST(LineCodecTest, IncompleteNoCrlfYet) {
    LineCodec codec(/*crlf=*/true);
    Line line;
    const std::string data = "partial";
    BufferView buf{reinterpret_cast<const uint8_t*>(data.data()), data.size()};
    auto status = codec.decode(buf, line);
    EXPECT_EQ(status, DecodeStatus::Incomplete);
}

TEST(LineCodecTest, EmptyLine) {
    LineCodec codec(/*crlf=*/true);
    Line line;
    const std::string data = "\r\n";
    BufferView buf{reinterpret_cast<const uint8_t*>(data.data()), data.size()};
    auto status = codec.decode(buf, line);
    EXPECT_EQ(status, DecodeStatus::Complete);
    EXPECT_EQ(line.data, "");
}

TEST(LineCodecTest, ZeroCopyViewPointsIntoBuffer) {
    LineCodec codec(/*crlf=*/true);
    Line line;
    const std::string data = "Host: example.com\r\n";
    BufferView buf{reinterpret_cast<const uint8_t*>(data.data()), data.size()};
    codec.decode(buf, line);
    // Zero-copy: the string_view data pointer should be within the source buffer
    EXPECT_GE(line.data.data(), data.data());
    EXPECT_LE(line.data.data(), data.data() + data.size());
}

TEST(LineCodecTest, ResetAllowsReuse) {
    LineCodec codec(/*crlf=*/true);
    Line line;
    const std::string data = "line1\r\n";
    BufferView buf{reinterpret_cast<const uint8_t*>(data.data()), data.size()};
    codec.decode(buf, line);
    codec.reset();

    const std::string data2 = "line2\r\n";
    BufferView buf2{reinterpret_cast<const uint8_t*>(data2.data()), data2.size()};
    auto status = codec.decode(buf2, line);
    EXPECT_EQ(status, DecodeStatus::Complete);
    EXPECT_EQ(line.data, "line2");
}
