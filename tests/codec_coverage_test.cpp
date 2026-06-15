/**
 * @file tests/codec_coverage_test.cpp
 * @brief Additional coverage for the qbuem::codec module.
 *
 * Complements tests/codec_test.cpp with NEW cases that the existing suite
 * does not exercise:
 *   - frame_codec.hpp : DecodeStatus enum identity, polymorphic dispatch through
 *                       IFrameCodec<Frame>* base pointer, virtual destructor.
 *   - line_codec.hpp  : incremental multi-line decode (buf advancement),
 *                       LF-mode retains trailing '\r', encode iovec byte content,
 *                       LF-mode 1-byte delimiter, leftover trailing bytes.
 *   - length_prefix_codec.hpp : 64 MiB DoS cap rejects oversize (Error),
 *                       at-cap boundary, zero-length payload, incremental header
 *                       + body across calls, arena (pmr) encode, buf advancement,
 *                       reset-after-error recovery.
 *   - http1_codec.hpp : request line + headers parse states (Complete),
 *                       Content-Length body, request smuggling (TE+CL) -> Error,
 *                       payload too large -> Error/413, empty buffer -> Incomplete,
 *                       headers_complete(), keep-alive reset reuse, encode()==0.
 *
 * All deterministic, single-process. No sockets, servers, or wall-clock sleeps.
 */

#include <gtest/gtest.h>

#include <qbuem/codec/frame_codec.hpp>
#include <qbuem/codec/http1_codec.hpp>
#include <qbuem/codec/length_prefix_codec.hpp>
#include <qbuem/codec/line_codec.hpp>

#include <arpa/inet.h>
#include <array>
#include <cstring>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

using namespace qbuem::codec;
using qbuem::BufferView;

namespace {

// Helper: build a BufferView over a std::string's bytes.
BufferView view_of(const std::string &s) {
    return BufferView{reinterpret_cast<const uint8_t *>(s.data()), s.size()};
}

// Helper: build a length-prefixed wire frame (4-byte big-endian header + body).
std::vector<uint8_t> make_wire(uint32_t length, const std::string &payload) {
    std::vector<uint8_t> wire;
    uint32_t net = htonl(length);
    const auto *hp = reinterpret_cast<const uint8_t *>(&net);
    wire.insert(wire.end(), hp, hp + 4);
    wire.insert(wire.end(),
                reinterpret_cast<const uint8_t *>(payload.data()),
                reinterpret_cast<const uint8_t *>(payload.data()) + payload.size());
    return wire;
}

} // namespace

// ════════════════════════════════════════════════════════════════════════════
// frame_codec.hpp — DecodeStatus + IFrameCodec polymorphism
// ════════════════════════════════════════════════════════════════════════════

TEST(FrameCodecTest, DecodeStatusValuesAreDistinct) {
    EXPECT_NE(DecodeStatus::Complete, DecodeStatus::Incomplete);
    EXPECT_NE(DecodeStatus::Complete, DecodeStatus::Error);
    EXPECT_NE(DecodeStatus::Incomplete, DecodeStatus::Error);
}

TEST(FrameCodecTest, PolymorphicDispatchThroughBasePointer) {
    // Use a concrete LineCodec through the abstract IFrameCodec<Line> interface.
    LineCodec concrete(/*crlf=*/true);
    IFrameCodec<Line> *base = &concrete;

    Line line;
    std::string data = "polymorphic\r\n";
    BufferView buf = view_of(data);

    DecodeStatus st = base->decode(buf, line);
    EXPECT_EQ(st, DecodeStatus::Complete);
    EXPECT_EQ(line.data, "polymorphic");

    // reset() is virtual; calling through base must not crash.
    base->reset();
}

TEST(FrameCodecTest, VirtualDestructorReleasesDerived) {
    // Deleting a derived object through a base pointer must call the
    // derived destructor (IFrameCodec has a virtual destructor).
    IFrameCodec<LengthPrefixedFrame> *codec = new LengthPrefixedCodec();
    LengthPrefixedFrame frame;
    frame.length = 3;
    frame.payload.resize(3);
    iovec vecs[2];
    EXPECT_EQ(codec->encode(frame, vecs, 2, nullptr), 2u);
    delete codec; // no leak / no crash under ASan
}

// ════════════════════════════════════════════════════════════════════════════
// line_codec.hpp — incremental decode, LF retains '\r', encode bytes, leftover
// ════════════════════════════════════════════════════════════════════════════

TEST(LineCodecCoverage, IncrementalMultiLineAdvancesBuffer) {
    LineCodec codec(/*crlf=*/true);
    std::string data = "first\r\nsecond\r\nthird\r\n";
    BufferView buf = view_of(data);

    Line l1, l2, l3;
    EXPECT_EQ(codec.decode(buf, l1), DecodeStatus::Complete);
    EXPECT_EQ(l1.data, "first");
    EXPECT_EQ(codec.decode(buf, l2), DecodeStatus::Complete);
    EXPECT_EQ(l2.data, "second");
    EXPECT_EQ(codec.decode(buf, l3), DecodeStatus::Complete);
    EXPECT_EQ(l3.data, "third");

    // Buffer fully consumed after the last line.
    EXPECT_EQ(buf.size(), 0u);
}

TEST(LineCodecCoverage, LeftoverBytesRemainAfterDecode) {
    LineCodec codec(/*crlf=*/false);
    std::string data = "line\nremainder-without-newline";
    BufferView buf = view_of(data);

    Line line;
    EXPECT_EQ(codec.decode(buf, line), DecodeStatus::Complete);
    EXPECT_EQ(line.data, "line");

    // The remaining (delimiter-less) bytes stay in buf and yield Incomplete.
    EXPECT_EQ(buf.size(), std::string("remainder-without-newline").size());
    Line line2;
    EXPECT_EQ(codec.decode(buf, line2), DecodeStatus::Incomplete);
}

TEST(LineCodecCoverage, LfModeKeepsTrailingCarriageReturn) {
    // In LF-only mode, a '\r' before '\n' is content, not part of the delimiter.
    LineCodec codec(/*crlf=*/false);
    std::string data = "value\r\n";
    BufferView buf = view_of(data);

    Line line;
    EXPECT_EQ(codec.decode(buf, line), DecodeStatus::Complete);
    EXPECT_EQ(line.data, "value\r"); // trailing '\r' retained
}

TEST(LineCodecCoverage, CrlfModeStripsOnlyMatchingCr) {
    // A lone '\n' (no preceding '\r') in CRLF mode keeps content verbatim.
    LineCodec codec(/*crlf=*/true);
    std::string data = "no-cr\n";
    BufferView buf = view_of(data);

    Line line;
    EXPECT_EQ(codec.decode(buf, line), DecodeStatus::Complete);
    EXPECT_EQ(line.data, "no-cr");
}

TEST(LineCodecCoverage, EncodeCrlfDelimiterBytes) {
    LineCodec codec(/*crlf=*/true);
    Line line;
    std::string content = "PONG";
    line.data = content;

    iovec vecs[2];
    size_t n = codec.encode(line, vecs, 2, nullptr);
    ASSERT_EQ(n, 2u);
    EXPECT_EQ(vecs[0].iov_len, 4u);
    EXPECT_EQ(std::memcmp(vecs[0].iov_base, "PONG", 4), 0);
    ASSERT_EQ(vecs[1].iov_len, 2u);
    EXPECT_EQ(std::memcmp(vecs[1].iov_base, "\r\n", 2), 0);
}

TEST(LineCodecCoverage, EncodeLfModeSingleByteDelimiter) {
    LineCodec codec(/*crlf=*/false);
    Line line;
    std::string content = "x";
    line.data = content;

    iovec vecs[2];
    size_t n = codec.encode(line, vecs, 2, nullptr);
    ASSERT_EQ(n, 2u);
    ASSERT_EQ(vecs[1].iov_len, 1u);
    EXPECT_EQ(std::memcmp(vecs[1].iov_base, "\n", 1), 0);
}

TEST(LineCodecCoverage, EncodeRejectsTooFewVecs) {
    LineCodec codec(/*crlf=*/true);
    Line line;
    std::string content = "data";
    line.data = content;

    iovec vecs[2];
    EXPECT_EQ(codec.encode(line, vecs, 0, nullptr), 0u);
    EXPECT_EQ(codec.encode(line, vecs, 1, nullptr), 0u);
}

// ════════════════════════════════════════════════════════════════════════════
// length_prefix_codec.hpp — DoS cap, boundaries, incremental, arena, recovery
// ════════════════════════════════════════════════════════════════════════════

TEST(LengthPrefixCoverage, OversizeFrameRejectedByDoSCap) {
    // A 4-byte attacker-controlled prefix larger than 64 MiB must return Error
    // BEFORE any large allocation is attempted.
    LengthPrefixedCodec codec;
    LengthPrefixedFrame frame;

    constexpr uint32_t kCap = 64u * 1024 * 1024;
    // Header announces cap + 1 bytes; provide no body — must still be Error.
    std::vector<uint8_t> wire = make_wire(kCap + 1, /*payload=*/"");
    BufferView buf{wire.data(), wire.size()};

    EXPECT_EQ(codec.decode(buf, frame), DecodeStatus::Error);
}

TEST(LengthPrefixCoverage, MaxUint32LengthRejected) {
    // 0xFFFFFFFF is wildly over the cap → Error (classic DoS prefix).
    LengthPrefixedCodec codec;
    LengthPrefixedFrame frame;
    std::vector<uint8_t> wire = {0xFF, 0xFF, 0xFF, 0xFF};
    BufferView buf{wire.data(), wire.size()};
    EXPECT_EQ(codec.decode(buf, frame), DecodeStatus::Error);
}

TEST(LengthPrefixCoverage, AtCapBoundaryIsNotRejected) {
    // Exactly 64 MiB is allowed (cap is strictly greater-than). With no body
    // bytes present the result is Incomplete, NOT Error — proving the boundary
    // condition uses `>` and not `>=`.
    LengthPrefixedCodec codec;
    LengthPrefixedFrame frame;
    constexpr uint32_t kCap = 64u * 1024 * 1024;
    std::vector<uint8_t> wire = make_wire(kCap, /*payload=*/"");
    BufferView buf{wire.data(), wire.size()};
    EXPECT_EQ(codec.decode(buf, frame), DecodeStatus::Incomplete);
}

TEST(LengthPrefixCoverage, ZeroLengthPayloadCompletes) {
    LengthPrefixedCodec codec;
    LengthPrefixedFrame frame;
    std::vector<uint8_t> wire = {0x00, 0x00, 0x00, 0x00}; // length=0, no payload
    BufferView buf{wire.data(), wire.size()};
    EXPECT_EQ(codec.decode(buf, frame), DecodeStatus::Complete);
    EXPECT_EQ(frame.length, 0u);
    EXPECT_TRUE(frame.payload.empty());
}

TEST(LengthPrefixCoverage, IncrementalHeaderAcrossTwoCalls) {
    LengthPrefixedCodec codec;
    LengthPrefixedFrame frame;

    // First feed: 2 of the 4 header bytes.
    std::vector<uint8_t> part1 = {0x00, 0x00};
    BufferView b1{part1.data(), part1.size()};
    EXPECT_EQ(codec.decode(b1, frame), DecodeStatus::Incomplete);
    EXPECT_EQ(b1.size(), 0u); // both header bytes consumed

    // Second feed: remaining 2 header bytes (length=4) + full 4-byte payload.
    std::vector<uint8_t> part2 = {0x00, 0x04, 'A', 'B', 'C', 'D'};
    BufferView b2{part2.data(), part2.size()};
    EXPECT_EQ(codec.decode(b2, frame), DecodeStatus::Complete);
    EXPECT_EQ(frame.length, 4u);
    ASSERT_EQ(frame.payload.size(), 4u);
    EXPECT_EQ(std::memcmp(frame.payload.data(), "ABCD", 4), 0);
}

TEST(LengthPrefixCoverage, IncrementalBodyAcrossTwoCalls) {
    LengthPrefixedCodec codec;
    LengthPrefixedFrame frame;

    // Full header (length=5) + first 2 payload bytes.
    std::vector<uint8_t> part1 = {0x00, 0x00, 0x00, 0x05, 'h', 'e'};
    BufferView b1{part1.data(), part1.size()};
    EXPECT_EQ(codec.decode(b1, frame), DecodeStatus::Incomplete);

    // Remaining 3 payload bytes.
    std::vector<uint8_t> part2 = {'l', 'l', 'o'};
    BufferView b2{part2.data(), part2.size()};
    EXPECT_EQ(codec.decode(b2, frame), DecodeStatus::Complete);
    ASSERT_EQ(frame.payload.size(), 5u);
    EXPECT_EQ(std::memcmp(frame.payload.data(), "hello", 5), 0);
}

TEST(LengthPrefixCoverage, BufferAdvancesPastFrameLeavingTrailingBytes) {
    // One complete frame followed by trailing bytes of a second frame's header.
    LengthPrefixedCodec codec;
    LengthPrefixedFrame frame;

    std::vector<uint8_t> wire = make_wire(2, "Hi");
    wire.push_back(0x00); // start of a next frame header (trailing)
    wire.push_back(0x00);
    BufferView buf{wire.data(), wire.size()};

    EXPECT_EQ(codec.decode(buf, frame), DecodeStatus::Complete);
    EXPECT_EQ(frame.length, 2u);
    // After consuming 4-byte header + 2-byte payload, 2 trailing bytes remain.
    EXPECT_EQ(buf.size(), 2u);
}

TEST(LengthPrefixCoverage, EncodeWithArenaHeaderBigEndian) {
    LengthPrefixedCodec codec;
    LengthPrefixedFrame frame;
    frame.length = 0x01020304u;
    frame.payload.resize(4);

    std::array<std::byte, 256> backing{};
    std::pmr::monotonic_buffer_resource arena{backing.data(), backing.size()};

    iovec vecs[2];
    size_t n = codec.encode(frame, vecs, 2, &arena);
    ASSERT_EQ(n, 2u);
    const auto *hdr = static_cast<const uint8_t *>(vecs[0].iov_base);
    EXPECT_EQ(hdr[0], 0x01);
    EXPECT_EQ(hdr[1], 0x02);
    EXPECT_EQ(hdr[2], 0x03);
    EXPECT_EQ(hdr[3], 0x04);
}

TEST(LengthPrefixCoverage, ResetAfterOversizeErrorRecovers) {
    LengthPrefixedCodec codec;
    LengthPrefixedFrame frame;

    // Trigger the DoS-cap Error path.
    std::vector<uint8_t> bad = {0xFF, 0xFF, 0xFF, 0xFF};
    BufferView b1{bad.data(), bad.size()};
    EXPECT_EQ(codec.decode(b1, frame), DecodeStatus::Error);

    // Note: the Error path returns before resetting state_ to Header, so a
    // reset() is required before the decoder can be reused. Verify recovery.
    codec.reset();
    std::vector<uint8_t> good = make_wire(2, "ok");
    BufferView b2{good.data(), good.size()};
    EXPECT_EQ(codec.decode(b2, frame), DecodeStatus::Complete);
    EXPECT_EQ(frame.length, 2u);
}

// ════════════════════════════════════════════════════════════════════════════
// http1_codec.hpp — request-line + header parse states, errors, keep-alive
// ════════════════════════════════════════════════════════════════════════════

TEST(Http1CodecCoverage, EmptyBufferIsIncomplete) {
    Http1Codec codec;
    qbuem::Request req;
    BufferView empty{static_cast<const uint8_t *>(nullptr), size_t{0}};
    EXPECT_EQ(codec.decode(empty, req), DecodeStatus::Incomplete);
}

TEST(Http1CodecCoverage, SimpleGetRequestLineAndHeaders) {
    Http1Codec codec;
    qbuem::Request req;
    std::string raw =
        "GET /index.html?a=1 HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "User-Agent: test\r\n"
        "\r\n";
    BufferView buf = view_of(raw);

    EXPECT_EQ(codec.decode(buf, req), DecodeStatus::Complete);
    EXPECT_EQ(req.method(), qbuem::Method::Get);
    EXPECT_EQ(req.path(), "/index.html");
    EXPECT_EQ(req.header("Host"), "example.com");
    EXPECT_EQ(req.header("User-Agent"), "test");
    EXPECT_TRUE(codec.headers_complete());
    // Whole request consumed.
    EXPECT_EQ(buf.size(), 0u);
}

TEST(Http1CodecCoverage, PostWithContentLengthBody) {
    Http1Codec codec;
    qbuem::Request req;
    std::string raw =
        "POST /submit HTTP/1.1\r\n"
        "Host: h\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";
    BufferView buf = view_of(raw);

    EXPECT_EQ(codec.decode(buf, req), DecodeStatus::Complete);
    EXPECT_EQ(req.method(), qbuem::Method::Post);
    EXPECT_EQ(req.path(), "/submit");
    EXPECT_EQ(req.body(), "hello");
}

TEST(Http1CodecCoverage, RequestSmugglingTeAndClRejected) {
    // RFC 7230 §3.3.3: both Transfer-Encoding and Content-Length present → 400.
    Http1Codec codec;
    qbuem::Request req;
    std::string raw =
        "POST / HTTP/1.1\r\n"
        "Host: h\r\n"
        "Content-Length: 4\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n";
    BufferView buf = view_of(raw);

    EXPECT_EQ(codec.decode(buf, req), DecodeStatus::Error);
    EXPECT_EQ(codec.error_status(), 400);
}

TEST(Http1CodecCoverage, MalformedContentLengthRejected) {
    Http1Codec codec;
    qbuem::Request req;
    std::string raw =
        "POST / HTTP/1.1\r\n"
        "Host: h\r\n"
        "Content-Length: notanumber\r\n"
        "\r\n";
    BufferView buf = view_of(raw);

    EXPECT_EQ(codec.decode(buf, req), DecodeStatus::Error);
    EXPECT_EQ(codec.error_status(), 400);
}

TEST(Http1CodecCoverage, PayloadTooLargeRejectedWith413) {
    Http1Codec codec;
    qbuem::Request req;
    // MAX_BODY_SIZE is 1 MiB; declare 2 MiB → 413.
    std::string raw =
        "POST / HTTP/1.1\r\n"
        "Host: h\r\n"
        "Content-Length: 2097152\r\n"
        "\r\n";
    BufferView buf = view_of(raw);

    EXPECT_EQ(codec.decode(buf, req), DecodeStatus::Error);
    EXPECT_EQ(codec.error_status(), 413);
}

TEST(Http1CodecCoverage, HeadersIncompleteWithoutTerminator) {
    Http1Codec codec;
    qbuem::Request req;
    // No blank-line terminator yet → Incomplete; codec consumes all bytes.
    std::string raw = "GET / HTTP/1.1\r\nHost: h\r\n";
    BufferView buf = view_of(raw);

    EXPECT_EQ(codec.decode(buf, req), DecodeStatus::Incomplete);
    EXPECT_FALSE(codec.headers_complete());
    EXPECT_EQ(buf.size(), 0u); // codec drains the buffer on Incomplete
}

TEST(Http1CodecCoverage, EncodeAlwaysReturnsZero) {
    // HTTP server codec does not encode requests.
    Http1Codec codec;
    qbuem::Request req;
    iovec vecs[4];
    EXPECT_EQ(codec.encode(req, vecs, 4, nullptr), 0u);
}

TEST(Http1CodecCoverage, ResetEnablesKeepAliveReuse) {
    Http1Codec codec;

    qbuem::Request req1;
    std::string raw1 = "GET /one HTTP/1.1\r\nHost: h\r\n\r\n";
    BufferView b1 = view_of(raw1);
    ASSERT_EQ(codec.decode(b1, req1), DecodeStatus::Complete);
    EXPECT_EQ(req1.path(), "/one");

    // Without reset() the parser is stuck Complete; reset() prepares the next.
    codec.reset();
    EXPECT_FALSE(codec.headers_complete());

    qbuem::Request req2;
    std::string raw2 = "DELETE /two HTTP/1.1\r\nHost: h\r\n\r\n";
    BufferView b2 = view_of(raw2);
    ASSERT_EQ(codec.decode(b2, req2), DecodeStatus::Complete);
    EXPECT_EQ(req2.method(), qbuem::Method::Delete);
    EXPECT_EQ(req2.path(), "/two");
}
