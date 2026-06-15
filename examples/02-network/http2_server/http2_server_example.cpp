/**
 * @file http2_server_example.cpp
 * @brief qbuem HTTP/2 server handler — in-memory frame + HPACK + request round-trip.
 *
 * Demonstrates the *server* side of HTTP/2 (`qbuem::Http2Handler`) end-to-end,
 * entirely in memory — no socket, no TLS, no reactor required. Every byte shown
 * below is genuinely computed by the library; nothing is faked.
 *
 * Coverage (all runs synchronously and produces real output):
 *   §1  HpackEncoder / HpackDecoder round-trip (RFC 7541 static table)
 *   §2  Http2Handler::serialize_frame — 9-byte wire header round-trip
 *   §3  Connection preface → server SETTINGS frame
 *   §4  Inbound client HEADERS frame → handle_frame → stream decoded
 *   §5  PING → PONG (ACK echo of the 8-byte opaque payload)
 *   §6  Response path: send_headers + send_data → drain → serialize to wire
 *
 * ─── Why this is "experimental" ───────────────────────────────────────────────
 * `Http2Handler` is a minimal implementation (see the header's own "Limitations"
 * note): no flow control, no HPACK Huffman, no dynamic table. In addition, the
 * application RequestHandler is invoked as a *detached* lazy `Task<>` — it is only
 * resumed by a live reactor/Dispatcher driving real socket I/O. In this standalone
 * in-memory demo there is no reactor to resume it, so §6 drives the *response path*
 * explicitly (exactly the calls a handler body would make: send_headers/send_data).
 * That is faithful to how the bytes go on the wire; it just skips the auto-dispatch
 * of the user callback. See `limitations` in the report for the honest gap.
 *
 * Zero-copy / zero-alloc idioms used: `std::span<const uint8_t>` for all buffer
 * passing, `Http2Handler::serialize_frame` writes contiguous wire bytes, headers
 * decoded in place from the frame payload span.
 */

#include <qbuem/server/http2_handler.hpp>

#include <qbuem/compat/print.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace qbuem;
using std::println;
using std::print;

// ─────────────────────────────────────────────────────────────────────────────
// Synchronous driver for the handler's lazy Task<T> coroutines.
//
// Http2Handler's frame methods return Task<T> with initial_suspend ==
// suspend_always. On this in-memory path they never suspend on real I/O — every
// co_await completes inline — so a single resume() runs them to completion. (A
// live server instead drives these from the Dispatcher event loop.)
// ─────────────────────────────────────────────────────────────────────────────

template <typename T>
static T sync_wait(Task<T> task) {
    task.handle.resume();
    return std::move(*task.handle.promise().value);
}

static void sync_wait_void(Task<void> task) {
    task.handle.resume();
    // Task<void> owns the frame and destroys it in its destructor here.
}

// ─── small helpers ────────────────────────────────────────────────────────────

static void hex_dump(std::string_view label, std::span<const uint8_t> bytes,
                     size_t max = 48) {
    print("  {:<22}", label);
    for (size_t i = 0; i < bytes.size() && i < max; ++i)
        print("{:02x} ", bytes[i]);
    if (bytes.size() > max) print("...");
    println(" ({} bytes)", bytes.size());
}

static int g_pass = 0;
static int g_fail = 0;
static void check(std::string_view what, bool ok) {
    println("  [{}] {}", ok ? "PASS" : "FAIL", what);
    if (ok) ++g_pass; else ++g_fail;
}

// ─────────────────────────────────────────────────────────────────────────────
// §1. HPACK encode / decode round-trip
// ─────────────────────────────────────────────────────────────────────────────

static void demo_hpack() {
    println("\n─── §1. HPACK encode/decode round-trip (RFC 7541) ─────────");

    HpackEncoder encoder;
    HpackDecoder decoder;

    std::unordered_map<std::string, std::string> request = {
        {":method",    "GET"},
        {":path",      "/"},
        {":scheme",    "https"},
        {":authority", "api.example.com"},
        {"user-agent", "qbuem-h2-demo/1.0"},
    };

    std::vector<uint8_t> block = encoder.encode(request);
    hex_dump("encoded block:", block);

    auto decoded = decoder.decode(std::span<const uint8_t>(block));

    println("  decoded {} headers from {} encoded bytes:", decoded.size(), block.size());
    for (auto& [k, v] : decoded)
        println("    {:<12} = {}", k, v);

    // Static-table indexed fields (:method GET, :path /, :scheme https) compress
    // to a single byte each; literal fields carry their value inline.
    check(":method round-trips",    decoded[":method"]    == "GET");
    check(":path round-trips",      decoded[":path"]      == "/");
    check(":scheme round-trips",    decoded[":scheme"]    == "https");
    check(":authority round-trips", decoded[":authority"] == "api.example.com");
    check("user-agent round-trips", decoded["user-agent"] == "qbuem-h2-demo/1.0");
}

// ─────────────────────────────────────────────────────────────────────────────
// §2. Frame wire serialization round-trip (RFC 7540 §4.1, 9-byte header)
// ─────────────────────────────────────────────────────────────────────────────

static void demo_frame_serialize() {
    println("\n─── §2. Frame serialize → 9-byte wire header ──────────────");

    Http2Frame frame;
    frame.type      = Http2FrameType::HEADERS;
    frame.stream_id = 1;
    frame.flags     = HTTP2_FLAG_END_HEADERS | HTTP2_FLAG_END_STREAM;
    frame.payload   = {0x82, 0x84, 0x86, 0x41, 0x0a}; // dummy HPACK-ish bytes
    frame.length    = static_cast<uint32_t>(frame.payload.size());

    std::vector<uint8_t> wire = Http2Handler::serialize_frame(frame);
    hex_dump("wire bytes:", wire);

    // Manually decode the 9-byte header to verify the bit layout.
    uint32_t len   = (uint32_t(wire[0]) << 16) | (uint32_t(wire[1]) << 8) | wire[2];
    uint8_t  type  = wire[3];
    uint8_t  flags = wire[4];
    uint32_t sid   = ((uint32_t(wire[5]) << 24) | (uint32_t(wire[6]) << 16) |
                      (uint32_t(wire[7]) << 8) | wire[8]) & 0x7FFFFFFF;

    println("  decoded header: len={} type=0x{:x} flags=0x{:x} stream_id={}",
            len, type, flags, sid);

    check("length encoded (24-bit BE)", len == frame.payload.size());
    check("type == HEADERS (0x1)",      type == 0x1);
    check("flags END_HEADERS|END_STREAM", flags == (HTTP2_FLAG_END_HEADERS | HTTP2_FLAG_END_STREAM));
    check("stream_id == 1",             sid == 1);
    check("total wire == 9 + payload",  wire.size() == 9 + frame.payload.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// §3. Connection preface → server SETTINGS
// ─────────────────────────────────────────────────────────────────────────────

static void demo_preface(Http2Handler& h2) {
    println("\n─── §3. Connection preface → server SETTINGS ──────────────");

    auto r = sync_wait(h2.send_connection_preface());
    auto frames = h2.drain_pending_frames();

    check("send_connection_preface ok", r.has_value());
    check("one SETTINGS frame queued",  frames.size() == 1);
    if (!frames.empty()) {
        const Http2Frame& s = frames.front();
        check("frame type == SETTINGS (0x4)", s.type == Http2FrameType::SETTINGS);
        check("connection-level (stream_id 0)", s.stream_id == 0);
        // 3 params × 6 bytes each = 18-byte payload.
        check("payload = 3 params × 6 bytes", s.payload.size() == 18);
        hex_dump("SETTINGS wire:", Http2Handler::serialize_frame(s));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// §4. Inbound client HEADERS frame → handle_frame → stream decoded
// ─────────────────────────────────────────────────────────────────────────────

static void demo_inbound_headers(Http2Handler& h2) {
    println("\n─── §4. Inbound client HEADERS → handle_frame ─────────────");

    // A client builds its request header block the same way (HPACK).
    HpackEncoder encoder;
    std::unordered_map<std::string, std::string> req = {
        {":method",    "POST"},
        {":path",      "/api/v1/events"},
        {":scheme",    "https"},
        {":authority", "api.example.com"},
        {"content-type", "application/json"},
    };
    std::vector<uint8_t> block = encoder.encode(req);

    Http2Frame headers_frame;
    headers_frame.type      = Http2FrameType::HEADERS;
    headers_frame.stream_id = 1;
    // END_HEADERS: complete block. (no END_STREAM yet — body follows in §4b)
    headers_frame.flags     = HTTP2_FLAG_END_HEADERS;
    headers_frame.payload   = block;
    headers_frame.length    = static_cast<uint32_t>(block.size());

    hex_dump("inbound HEADERS wire:",
             Http2Handler::serialize_frame(headers_frame));

    auto r = sync_wait(h2.handle_frame(std::move(headers_frame)));
    check("handle_frame(HEADERS) ok", r.has_value());
    check("no error frames emitted",  h2.drain_pending_frames().empty());

    // §4b. A DATA frame carrying the JSON body, with END_STREAM.
    std::string json = R"({"type":"demo","ok":true})";
    Http2Frame data_frame;
    data_frame.type      = Http2FrameType::DATA;
    data_frame.stream_id = 1;
    data_frame.flags     = HTTP2_FLAG_END_STREAM;
    data_frame.payload.assign(json.begin(), json.end());
    data_frame.length    = static_cast<uint32_t>(data_frame.payload.size());

    println("  inbound DATA: '{}' ({} bytes, END_STREAM)", json, json.size());

    auto rd = sync_wait(h2.handle_frame(std::move(data_frame)));
    check("handle_frame(DATA, END_STREAM) ok", rd.has_value());
    // The handler is invoked as a detached lazy Task here; with no reactor to
    // resume it, the user callback body does not run. The stream state and the
    // collected request body are nonetheless materialized inside the handler.
    // (See limitations.) handle_frame itself completes without protocol error.
    check("DATA accepted without RST/GOAWAY", h2.drain_pending_frames().empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// §5. PING → PONG (ACK echo of opaque payload)
// ─────────────────────────────────────────────────────────────────────────────

static void demo_ping(Http2Handler& h2) {
    println("\n─── §5. PING → PONG (8-byte opaque echo) ──────────────────");

    Http2Frame ping;
    ping.type      = Http2FrameType::PING;
    ping.stream_id = 0;
    ping.flags     = 0; // not ACK — server must reply
    ping.payload   = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
    ping.length    = 8;
    hex_dump("inbound PING:", ping.payload);

    auto r = sync_wait(h2.handle_frame(std::move(ping)));
    auto out = h2.drain_pending_frames();

    check("handle_frame(PING) ok", r.has_value());
    check("one PONG frame queued",  out.size() == 1);
    if (!out.empty()) {
        const Http2Frame& pong = out.front();
        check("PONG type == PING (0x6)",  pong.type == Http2FrameType::PING);
        check("PONG has ACK flag",        (pong.flags & HTTP2_FLAG_ACK) != 0);
        const std::vector<uint8_t> expect = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
        check("PONG echoes opaque data",  pong.payload == expect);
        hex_dump("PONG payload:", pong.payload);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// §6. Response path — what a handler body emits onto the wire
// ─────────────────────────────────────────────────────────────────────────────

static void demo_response(Http2Handler& h2) {
    println("\n─── §6. Response: send_headers + send_data → wire ─────────");

    // Exactly the two calls an application handler makes to answer stream 1.
    std::unordered_map<std::string, std::string> resp_headers = {
        {":status",      "200"},
        {"content-type", "text/plain"},
    };
    auto sh = sync_wait(h2.send_headers(1, resp_headers, /*end_stream=*/false));
    check("send_headers ok", sh.has_value());

    std::string body = "Hello from qbuem HTTP/2 server!";
    std::vector<uint8_t> body_bytes(body.begin(), body.end());
    auto sd = sync_wait(h2.send_data(
        1, std::span<const uint8_t>(body_bytes), /*end_stream=*/true));
    check("send_data ok", sd.has_value());

    auto frames = h2.drain_pending_frames();
    check("two response frames (HEADERS + DATA)", frames.size() == 2);

    for (const Http2Frame& f : frames) {
        std::vector<uint8_t> wire = Http2Handler::serialize_frame(f);
        const char* tname = (f.type == Http2FrameType::HEADERS) ? "HEADERS"
                          : (f.type == Http2FrameType::DATA)    ? "DATA" : "?";
        println("  → {:<8} stream={} flags=0x{:x} {} wire bytes",
                tname, f.stream_id, f.flags, wire.size());
        hex_dump("    bytes:", wire);
    }

    if (frames.size() == 2) {
        check("first is HEADERS",          frames[0].type == Http2FrameType::HEADERS);
        check("second is DATA",            frames[1].type == Http2FrameType::DATA);
        check("DATA has END_STREAM",       (frames[1].flags & HTTP2_FLAG_END_STREAM) != 0);
        // The DATA payload is the response body verbatim (no copy beyond span->vec).
        std::string echoed(frames[1].payload.begin(), frames[1].payload.end());
        check("DATA payload == body",      echoed == body);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    println("╔══════════════════════════════════════════════════════════╗");
    println("║   qbuem-stack HTTP/2 Server Handler Demo (in-memory)      ║");
    println("╚══════════════════════════════════════════════════════════╝");
    println("Experimental handler: no flow control, no HPACK Huffman,");
    println("no dynamic table. Every byte below is computed, not faked.");

    // One handler instance threads §3-§6 through a single connection's state.
    // The RequestHandler is registered for completeness; in a live server the
    // Dispatcher resumes it. Standalone, §6 drives the response path directly.
    Http2Handler h2(
        [](std::unordered_map<std::string, std::string> headers,
           std::vector<uint8_t> body,
           std::shared_ptr<Http2Stream> stream) -> Task<void> {
            // Would run under a live reactor — kept here as the real signature.
            (void)headers; (void)body; (void)stream;
            co_return;
        });

    demo_hpack();             // standalone, no handler state
    demo_frame_serialize();   // standalone
    demo_preface(h2);
    demo_inbound_headers(h2);
    demo_ping(h2);
    demo_response(h2);

    println("\n─── Summary ───────────────────────────────────────────────");
    println("  {} checks PASS, {} FAIL", g_pass, g_fail);
    println("");

    return g_fail == 0 ? 0 : 1;
}
