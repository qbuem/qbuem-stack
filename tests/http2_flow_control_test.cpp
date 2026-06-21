/**
 * @file tests/http2_flow_control_test.cpp
 * @brief Behavioral tests for HTTP/2 SETTINGS negotiation + flow control
 *        (RFC 7540 §6.5, §6.9) on Http2Handler.
 *
 * Part of the SaaS-readiness build-out: the handler is moving from a frame/HPACK
 * codec toward a real h2c server (TLS terminated at the edge). These tests drive
 * the handler in-memory — its frame coroutines never suspend on real I/O, so a
 * single resume() runs each to completion (mirrors the standalone example).
 */

#include <qbuem/core/task.hpp>
#include <qbuem/server/http2_handler.hpp>

#include <cstdint>
#include <gtest/gtest.h>
#include <unordered_map>
#include <vector>

using namespace qbuem;

namespace {

// ── Synchronous driver for the handler's lazy Task<T> coroutines (in-memory). ──
template <typename T>
T sync_wait(Task<T> task) {
    task.handle.resume();
    return std::move(*task.handle.promise().value);
}

// ── Frame builders ────────────────────────────────────────────────────────────

void put_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>(x & 0xFF));
}
void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>(x & 0xFF));
}

Http2Frame settings_frame(
    const std::vector<std::pair<uint16_t, uint32_t>>& params) {
    Http2Frame f;
    f.type = Http2FrameType::SETTINGS;
    f.stream_id = 0;
    f.flags = 0;
    for (auto& [id, val] : params) {
        put_u16(f.payload, id);
        put_u32(f.payload, val);
    }
    f.length = static_cast<uint32_t>(f.payload.size());
    return f;
}

Http2Frame window_update_frame(uint32_t stream_id, uint32_t increment) {
    Http2Frame f;
    f.type = Http2FrameType::WINDOW_UPDATE;
    f.stream_id = stream_id;
    f.flags = 0;
    put_u32(f.payload, increment);
    f.length = 4;
    return f;
}

// Open a real stream by feeding a minimal HEADERS frame (HPACK-encoded).
Http2Frame headers_frame(uint32_t stream_id, bool end_stream) {
    HpackEncoder enc;
    std::unordered_map<std::string, std::string> h = {
        {":method", "GET"}, {":path", "/"}, {":scheme", "http"},
        {":authority", "x"}};
    Http2Frame f;
    f.type = Http2FrameType::HEADERS;
    f.stream_id = stream_id;
    f.flags = HTTP2_FLAG_END_HEADERS;
    if (end_stream) f.flags |= HTTP2_FLAG_END_STREAM;
    f.payload = enc.encode(h);
    f.length = static_cast<uint32_t>(f.payload.size());
    return f;
}

Http2Frame data_frame(uint32_t stream_id, size_t n, bool end_stream) {
    Http2Frame f;
    f.type = Http2FrameType::DATA;
    f.stream_id = stream_id;
    f.flags = end_stream ? HTTP2_FLAG_END_STREAM : 0;
    f.payload.assign(n, 0x41);
    f.length = static_cast<uint32_t>(n);
    return f;
}

Http2Handler make_handler() {
    return Http2Handler([](std::unordered_map<std::string, std::string>,
                          std::vector<uint8_t>,
                          std::shared_ptr<Http2Stream>) -> Task<void> {
        co_return;
    });
}

bool drain_has(Http2Handler& h2, Http2FrameType type, uint8_t flags_mask = 0) {
    for (auto& f : h2.drain_pending_frames())
        if (f.type == type && (flags_mask == 0 || (f.flags & flags_mask)))
            return true;
    return false;
}

} // namespace

// ── SETTINGS negotiation ────────────────────────────────────────────────────

TEST(Http2FlowControl, SettingsNegotiationAppliesPeerValues) {
    auto h2 = make_handler();
    auto r = sync_wait(h2.handle_frame(settings_frame({
        {0x4 /*INITIAL_WINDOW_SIZE*/, 100000},
        {0x5 /*MAX_FRAME_SIZE*/, 32768},
        {0x2 /*ENABLE_PUSH*/, 0},
    })));
    EXPECT_TRUE(r.has_value());
    EXPECT_TRUE(h2.peer_settings_received());
    EXPECT_EQ(h2.peer_initial_window_size(), 100000u);
    EXPECT_EQ(h2.peer_max_frame_size(), 32768u);
    // We must ACK the applied SETTINGS.
    EXPECT_TRUE(drain_has(h2, Http2FrameType::SETTINGS, HTTP2_FLAG_ACK));
}

TEST(Http2FlowControl, SettingsRejectsBadMaxFrameSize) {
    auto h2 = make_handler();
    auto r = sync_wait(h2.handle_frame(settings_frame({{0x5, 1000}}))); // < 2^14
    EXPECT_FALSE(r.has_value());
    EXPECT_TRUE(drain_has(h2, Http2FrameType::GOAWAY));
}

TEST(Http2FlowControl, SettingsRejectsMalformedLength) {
    auto h2 = make_handler();
    Http2Frame f;
    f.type = Http2FrameType::SETTINGS;
    f.stream_id = 0;
    f.payload = {0x00, 0x04, 0x00, 0x00, 0x10}; // 5 bytes — not a multiple of 6
    f.length = 5;
    auto r = sync_wait(h2.handle_frame(std::move(f)));
    EXPECT_FALSE(r.has_value());
    EXPECT_TRUE(drain_has(h2, Http2FrameType::GOAWAY));
}

TEST(Http2FlowControl, SettingsAckWithPayloadIsError) {
    auto h2 = make_handler();
    Http2Frame f;
    f.type = Http2FrameType::SETTINGS;
    f.stream_id = 0;
    f.flags = HTTP2_FLAG_ACK;
    f.payload = {0x00};
    f.length = 1;
    auto r = sync_wait(h2.handle_frame(std::move(f)));
    EXPECT_FALSE(r.has_value());
}

TEST(Http2FlowControl, InitialWindowSizeAdjustsExistingStreams) {
    auto h2 = make_handler();
    // Open stream 1 first; its SEND window starts at the default peer initial (65535).
    (void)sync_wait(h2.handle_frame(headers_frame(1, /*end_stream=*/false)));
    h2.drain_pending_frames();
    EXPECT_EQ(h2.stream_send_window(1), 65535);
    // Peer raises INITIAL_WINDOW_SIZE → existing stream window shifts by the delta.
    auto r = sync_wait(h2.handle_frame(settings_frame({{0x4, 70000}})));
    EXPECT_TRUE(r.has_value());
    EXPECT_EQ(h2.stream_send_window(1), 70000); // 65535 + (70000 - 65535)
}

// ── WINDOW_UPDATE ───────────────────────────────────────────────────────────

TEST(Http2FlowControl, WindowUpdateGrowsSendWindows) {
    auto h2 = make_handler();
    (void)sync_wait(h2.handle_frame(headers_frame(1, false)));
    h2.drain_pending_frames();

    EXPECT_EQ(h2.connection_send_window(), 65535);
    (void)sync_wait(h2.handle_frame(window_update_frame(0, 1000)));
    EXPECT_EQ(h2.connection_send_window(), 66535);

    EXPECT_EQ(h2.stream_send_window(1), 65535);
    (void)sync_wait(h2.handle_frame(window_update_frame(1, 500)));
    EXPECT_EQ(h2.stream_send_window(1), 66035);
}

TEST(Http2FlowControl, WindowUpdateZeroIncrementIsError) {
    auto h2 = make_handler();
    // Stream 0 (connection) zero increment → connection error (GOAWAY).
    auto r = sync_wait(h2.handle_frame(window_update_frame(0, 0)));
    EXPECT_FALSE(r.has_value());
    EXPECT_TRUE(drain_has(h2, Http2FrameType::GOAWAY));

    // Stream-level zero increment → stream error (RST_STREAM), connection survives.
    auto h2b = make_handler();
    (void)sync_wait(h2b.handle_frame(headers_frame(1, false)));
    h2b.drain_pending_frames();
    auto r2 = sync_wait(h2b.handle_frame(window_update_frame(1, 0)));
    EXPECT_TRUE(r2.has_value());
    EXPECT_TRUE(drain_has(h2b, Http2FrameType::RST_STREAM));
}

// ── Receive-side flow control ───────────────────────────────────────────────

TEST(Http2FlowControl, ReceiveDataConsumesAndReplenishesWindow) {
    auto h2 = make_handler();
    (void)sync_wait(h2.handle_frame(headers_frame(1, /*end_stream=*/false)));
    h2.drain_pending_frames();
    EXPECT_EQ(h2.connection_recv_window(), 65535);

    // Send 40000 bytes — drops both windows below half (32767), triggering a
    // WINDOW_UPDATE replenishment back to the initial size.
    auto r = sync_wait(h2.handle_frame(data_frame(1, 40000, /*end_stream=*/false)));
    EXPECT_TRUE(r.has_value());
    EXPECT_EQ(h2.connection_recv_window(), 65535); // replenished
    EXPECT_TRUE(drain_has(h2, Http2FrameType::WINDOW_UPDATE));
}

TEST(Http2FlowControl, SendDataEnforcesWindow) {
  auto h2 = make_handler();
  // Peer advertises a tiny initial window → new stream's SEND window = 10.
  (void)sync_wait(h2.handle_frame(settings_frame({{0x4 /*INITIAL_WINDOW_SIZE*/, 10}})));
  h2.drain_pending_frames();
  (void)sync_wait(h2.handle_frame(headers_frame(1, /*end_stream=*/false)));
  h2.drain_pending_frames();
  ASSERT_EQ(h2.stream_send_window(1), 10);

  const std::vector<uint8_t> five(5, 0x41), ten(10, 0x41);
  auto r1 = sync_wait(h2.send_data(1, std::span<const uint8_t>(five), false));
  EXPECT_TRUE(r1.has_value());            // 5 <= 10 → sent
  EXPECT_EQ(h2.stream_send_window(1), 5);

  auto r2 = sync_wait(h2.send_data(1, std::span<const uint8_t>(ten), false));
  EXPECT_FALSE(r2.has_value());           // 10 > remaining 5 → refused (would_block)
  EXPECT_EQ(h2.stream_send_window(1), 5); // window unchanged — frame not emitted
}

TEST(Http2FlowControl, ReceiveExceedingConnectionWindowGoesAway) {
    auto h2 = make_handler();
    (void)sync_wait(h2.handle_frame(headers_frame(1, false)));
    h2.drain_pending_frames();
    // 70000 > the 65535 connection receive window → connection FLOW_CONTROL_ERROR.
    auto r = sync_wait(h2.handle_frame(data_frame(1, 70000, false)));
    EXPECT_FALSE(r.has_value());
    EXPECT_TRUE(drain_has(h2, Http2FrameType::GOAWAY));
}
