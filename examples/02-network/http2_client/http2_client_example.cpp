/**
 * @file http2_client_example.cpp
 * @brief qbuem HTTP/2 client — multiplexed request/response over a single TCP connection.
 *
 * Demonstrates:
 *   1. HTTP/2 connection establishment (connection preface + SETTINGS)
 *   2. Sequential GET requests on the same connection
 *   3. POST request with JSON body
 *   4. HPACK header encoding / decoding
 *   5. Http2Connection frame-level operations
 *   6. Error handling and GOAWAY
 *
 * Start an HTTP/2-capable server before running:
 *   h2o --port 8080             (h2o supports HTTP/2 over cleartext)
 *   nghttpd 8080 --no-tls       (nghttp2 tools)
 *   python3 -m hyper.contrib     (Python hyper-h2 test server)
 *
 * The example falls back to printing a demo explanation when no server is reachable.
 */

#include <qbuem/http/http2_client.hpp>
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>

#include <chrono>
#include <qbuem/compat/print.hpp>
#include <stop_token>
#include <thread>

using namespace qbuem;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// §1. Protocol overview (always shown)
// ─────────────────────────────────────────────────────────────────────────────

void print_protocol_overview() {
    std::println("╔══════════════════════════════════════════════════════════╗");
    std::println("║       qbuem-stack HTTP/2 Client Demo                     ║");
    std::println("╚══════════════════════════════════════════════════════════╝");
    std::println("");
    std::println("HTTP/2 key improvements over HTTP/1.1:");
    std::println("  • Binary framing layer — no text parsing overhead");
    std::println("  • Multiplexing — N concurrent requests on 1 TCP connection");
    std::println("  • Header compression (HPACK) — reduces per-request overhead");
    std::println("  • Server push — proactively send resources");
    std::println("  • Stream prioritisation — important requests first");
    std::println("");
    std::println("Frame types implemented:");

    // Print all H2FrameType variants
    constexpr struct { std::string_view name; uint8_t code; std::string_view desc; } kFrames[] = {
        {"DATA",         0x0, "Stream payload data"},
        {"HEADERS",      0x1, "HPACK-compressed header block"},
        {"PRIORITY",     0x2, "Stream dependency and weight"},
        {"RST_STREAM",   0x3, "Immediate stream termination"},
        {"SETTINGS",     0x4, "Connection configuration"},
        {"PUSH_PROMISE", 0x5, "Server push notification"},
        {"PING",         0x6, "Liveness check / RTT measurement"},
        {"GOAWAY",       0x7, "Graceful connection close"},
        {"WINDOW_UPDATE",0x8, "Flow control window increment"},
        {"CONTINUATION", 0x9, "HEADERS continuation block"},
    };
    for (auto& f : kFrames)
        std::println("  0x{:x}  {:<14}  {}", f.code, f.name, f.desc);
}

// ─────────────────────────────────────────────────────────────────────────────
// §2. HPACK encoding demo (always runs — no network required)
// ─────────────────────────────────────────────────────────────────────────────

void demo_hpack() {
    std::println("\n─── §2. HPACK header encoding ─────────────────────────────");

    std::vector<std::byte> block;
    std::vector<std::pair<std::string,std::string>> extra = {
        {"accept",       "application/json"},
        {"x-request-id", "demo-001"},
    };
    hpack_encode_request(block, "GET", "/api/v1/users", "api.example.com",
                         std::span<const std::pair<std::string,std::string>>(extra));

    std::println("  Encoded HPACK block for GET /api/v1/users:");
    std::println("  Size: {} bytes (vs ~120 bytes plain text headers)", block.size());

    // Show hex dump of first 32 bytes
    std::print("  Bytes: ");
    for (size_t i = 0; i < std::min(block.size(), size_t{32}); ++i)
        std::print("{:02x} ", static_cast<uint8_t>(block[i]));
    if (block.size() > 32) std::print("...");
    std::println("");

    // Static index lookup
    std::println("  Static table index: :method GET  = {}", hpack_static_index(":method", "GET"));
    std::println("  Static table index: :method POST = {}", hpack_static_index(":method", "POST"));
    std::println("  Static table index: :path  /     = {}", hpack_static_index(":path",   "/"));
}

// ─────────────────────────────────────────────────────────────────────────────
// §3. Frame header encoding/decoding round-trip
// ─────────────────────────────────────────────────────────────────────────────

void demo_frame_header() {
    std::println("\n─── §3. Frame header wire encoding round-trip ─────────────");

    // Encode a HEADERS frame for stream 1 with EndHeaders flag
    std::array<std::byte, 9> hdr{};
    h2_encode_frame_header(std::span<std::byte,9>(hdr), 42,
                           H2FrameType::Headers,
                           H2Flags::EndHeaders, 1);

    std::print("  Encoded 9-byte frame header: ");
    for (auto b : hdr) std::print("{:02x} ", static_cast<uint8_t>(b));
    std::println("");

    // Decode back
    auto [len, type, flags, sid] = h2_decode_frame_header(
        std::span<const std::byte,9>(hdr));

    std::println("  Decoded:");
    std::println("    length    = {} (expected 42)", len);
    std::println("    type      = 0x{:x} HEADERS (expected 0x1)", std::to_underlying(type));
    std::println("    flags     = 0x{:x} END_HEADERS (expected 0x4)", flags);
    std::println("    stream_id = {} (expected 1)", sid);
    std::println("  Round-trip {}",
                 (len == 42 && type == H2FrameType::Headers &&
                  flags == H2Flags::EndHeaders && sid == 1) ? "PASS" : "FAIL");
}

// ─────────────────────────────────────────────────────────────────────────────
// §4. Live HTTP/2 connection (requires server on :8080)
// ─────────────────────────────────────────────────────────────────────────────

Task<void> demo_live_connection(std::stop_token st) {
    std::println("\n─── §4. Live HTTP/2 connection (requires server on :8080) ─");

    Http2Client client;
    auto conn = co_await client.connect("http://localhost:8080", st);
    if (!conn) {
        std::println("  [skip] no HTTP/2 server on :8080 ({})",
                     conn.error().message());
        std::println("  To run: nghttpd 8080 --no-tls  or  h2o");
        co_return;
    }

    std::println("  Connection established. Stream ID counter starts at 1.");

    // Sequential GET requests (reuse the same TCP connection)
    for (auto& path : {"/api/v1/status", "/api/v1/version", "/api/v1/health"}) {
        auto r = co_await (*conn)->get(path, st);
        if (!r) {
            std::println("  GET {} failed: {}", path, r.error().message());
            continue;
        }
        std::println("  GET {:25} → status={} body_len={}",
                     path, r->status, r->body.size());
    }

    // POST request with JSON body
    std::vector<std::pair<std::string,std::string>> hdrs = {
        {"content-type", "application/json"},
    };
    auto post_r = co_await (*conn)->post(
        "/api/v1/events",
        R"({"type":"demo","source":"http2_client_example"})",
        st, hdrs);

    if (post_r)
        std::println("  POST /api/v1/events → status={} ok={}",
                     post_r->status, post_r->ok());
    else
        std::println("  POST failed: {}", post_r.error().message());
}

// ─────────────────────────────────────────────────────────────────────────────
// §5. Simulate concurrent requests (channel-based parallelism demo)
// ─────────────────────────────────────────────────────────────────────────────

Task<void> demo_concurrent_streams(std::stop_token st) {
    std::println("\n─── §5. Concurrent stream simulation ──────────────────────");

    // Without a live server, demonstrate the stream ID allocation pattern
    std::println("  HTTP/2 stream ID allocation (client-initiated, odd IDs):");
    uint32_t sid = 1;
    for (int i = 0; i < 5; ++i, sid += 2)
        std::println("  request {:1} → stream_id={}", i + 1, sid);

    std::println("");
    std::println("  Multiplexing advantage over HTTP/1.1:");
    std::println("  • HTTP/1.1: 5 requests need 5 TCP connections (with pooling)");
    std::println("  • HTTP/2:   5 requests share 1 TCP connection — 1 handshake");
    std::println("  • With 10ms RTT: HTTP/1.1 = 50ms overhead, HTTP/2 = 10ms");
    co_return;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

Task<void> run_all(std::stop_token st) {
    print_protocol_overview();
    demo_hpack();
    demo_frame_header();
    co_await demo_live_connection(st);
    co_await demo_concurrent_streams(st);
    std::println("\n Done.\n");
}

int main() {
    Dispatcher d(2);
    std::jthread t([&d]{ d.run(); });

    std::stop_source ss;
    d.spawn(run_all(ss.get_token()));

    std::this_thread::sleep_for(std::chrono::seconds{3});
    ss.request_stop();
    d.stop();
    return 0;
}
