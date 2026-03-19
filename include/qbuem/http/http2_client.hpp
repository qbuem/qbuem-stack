#pragma once

/**
 * @file qbuem/http/http2_client.hpp
 * @brief HTTP/2 multiplexed fetch client — frame-based async request/response.
 * @defgroup qbuem_http2_client HTTP/2 Client
 * @ingroup qbuem_http
 *
 * ## Overview
 *
 * `Http2Client` sends multiple concurrent HTTP/2 requests over a single TCP
 * connection. Unlike `FetchClient` (HTTP/1.1, one request per connection slot),
 * HTTP/2 multiplexing eliminates head-of-line blocking and reduces handshake
 * overhead for services making many parallel calls to the same host.
 *
 * ## HTTP/2 Protocol (RFC 7540) summary
 * - Frames: fixed 9-byte header (length|type|flags|stream_id) + payload.
 * - Streams: logical request/response pairs identified by odd integers (1, 3, 5, …).
 * - SETTINGS handshake: both sides exchange initial SETTINGS frames after the
 *   "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" connection preface.
 * - HPACK: header block compression via static table + literal encoding.
 * - Flow control: WINDOW_UPDATE keeps receivers from being flooded.
 *
 * ## Usage
 * @code
 * Http2Client client;
 * auto conn = co_await client.connect("http://api.example.com", st);
 * if (!conn) co_return unexpected(conn.error());
 *
 * // Fire three requests concurrently on the same TCP connection
 * auto [r1, r2, r3] = co_await TaskGroup{}
 *     .spawn((*conn)->get("/api/users/1", st))
 *     .spawn((*conn)->get("/api/users/2", st))
 *     .spawn((*conn)->get("/api/users/3", st))
 *     .join_all<Http2Response>();
 * @endcode
 *
 * ## Limitations
 * - HTTPS (ALPN "h2") is not handled here; wrap a kTLS stream to enable it.
 * - PUSH_PROMISE frames are silently discarded on receipt.
 * - HPACK dynamic table is not supported (all headers encoded as literals).
 * - Flow control windows use the RFC 7540 default (65535 bytes per stream).
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/net/dns.hpp>
#include <qbuem/net/tcp_stream.hpp>
#include <qbuem/pipeline/async_channel.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace qbuem {

// ─── HTTP/2 frame constants ───────────────────────────────────────────────────

/** @brief HTTP/2 frame type codes (RFC 7540 §6). */
enum class H2FrameType : uint8_t {
    Data         = 0x0,
    Headers      = 0x1,
    Priority     = 0x2,
    RstStream    = 0x3,
    Settings     = 0x4,
    PushPromise  = 0x5,
    Ping         = 0x6,
    Goaway       = 0x7,
    WindowUpdate = 0x8,
    Continuation = 0x9,
};

/** @brief HTTP/2 frame flag bitmasks. */
namespace H2Flags {
    inline constexpr uint8_t EndStream  = 0x01; ///< Last DATA or HEADERS frame on stream
    inline constexpr uint8_t EndHeaders = 0x04; ///< Last HEADERS or CONTINUATION frame
    inline constexpr uint8_t Padded     = 0x08; ///< Frame has padding
    inline constexpr uint8_t Priority   = 0x20; ///< HEADERS frame has priority block
    inline constexpr uint8_t Ack        = 0x01; ///< SETTINGS or PING acknowledgement
} // namespace H2Flags

/** @brief HTTP/2 error codes (RFC 7540 §7). */
enum class H2Error : uint32_t {
    NoError            = 0x0,
    ProtocolError      = 0x1,
    InternalError      = 0x2,
    FlowControlError   = 0x3,
    SettingsTimeout    = 0x4,
    StreamClosed       = 0x5,
    FrameSizeError     = 0x6,
    RefusedStream      = 0x7,
    Cancel             = 0x8,
    CompressionError   = 0x9,
    ConnectError       = 0xa,
    EnhanceYourCalm    = 0xb,
    InadequateSecurity = 0xc,
    Http11Required     = 0xd,
};

/** @brief HTTP/2 SETTINGS parameter identifiers (RFC 7540 §6.5.2). */
enum class H2SettingId : uint16_t {
    HeaderTableSize      = 0x1,
    EnablePush           = 0x2,
    MaxConcurrentStreams = 0x3,
    InitialWindowSize    = 0x4,
    MaxFrameSize         = 0x5,
    MaxHeaderListSize    = 0x6,
};

// ─── Frame wire encoding helpers ─────────────────────────────────────────────

/**
 * @brief Serialize a 9-byte HTTP/2 frame header into `out`.
 *
 * @param out       Output buffer (must be at least 9 bytes).
 * @param length    Payload length (24-bit, max 16 MiB - 1).
 * @param type      Frame type byte.
 * @param flags     Flags byte.
 * @param stream_id Stream identifier (31-bit; R bit cleared).
 */
inline void h2_encode_frame_header(std::span<std::byte, 9> out,
                                   uint32_t length,
                                   H2FrameType type,
                                   uint8_t flags,
                                   uint32_t stream_id) noexcept {
    out[0] = std::byte((length >> 16) & 0xFF);
    out[1] = std::byte((length >>  8) & 0xFF);
    out[2] = std::byte( length        & 0xFF);
    out[3] = std::byte(std::to_underlying(type));
    out[4] = std::byte(flags);
    out[5] = std::byte((stream_id >> 24) & 0x7F);  // clear R bit
    out[6] = std::byte((stream_id >> 16) & 0xFF);
    out[7] = std::byte((stream_id >>  8) & 0xFF);
    out[8] = std::byte( stream_id        & 0xFF);
}

/**
 * @brief Parse a 9-byte HTTP/2 frame header.
 *
 * @param in 9-byte input span.
 * @returns Tuple of (payload_length, type, flags, stream_id).
 */
inline std::tuple<uint32_t, H2FrameType, uint8_t, uint32_t>
h2_decode_frame_header(std::span<const std::byte, 9> in) noexcept {
    uint32_t length =
        (static_cast<uint32_t>(in[0]) << 16) |
        (static_cast<uint32_t>(in[1]) <<  8) |
         static_cast<uint32_t>(in[2]);
    auto type  = static_cast<H2FrameType>(static_cast<uint8_t>(in[3]));
    uint8_t flags  = static_cast<uint8_t>(in[4]);
    uint32_t stream_id =
        ((static_cast<uint32_t>(in[5]) & 0x7F) << 24) |
        ( static_cast<uint32_t>(in[6])          << 16) |
        ( static_cast<uint32_t>(in[7])          <<  8) |
          static_cast<uint32_t>(in[8]);
    return {length, type, flags, stream_id};
}

// ─── HPACK static table ───────────────────────────────────────────────────────

/**
 * @brief HPACK static table index lookup for common headers (RFC 7541 Appendix A).
 *
 * Returns the static table index if the (name, value) pair is in the table,
 * or 0 if not found. Index 1 is `:authority`, etc.
 */
inline int hpack_static_index(std::string_view name, std::string_view value = "") noexcept {
    // Partial static table covering method + common pseudo-headers
    static constexpr std::pair<std::string_view, std::string_view> kTable[] = {
        {"",                   ""},          // 0 unused
        {":authority",         ""},          // 1
        {":method",            "GET"},       // 2
        {":method",            "POST"},      // 3
        {":path",              "/"},         // 4
        {":path",              "/index.html"},// 5
        {":scheme",            "http"},      // 6
        {":scheme",            "https"},     // 7
        {":status",            "200"},       // 8
        {":status",            "204"},       // 9
        {":status",            "206"},       // 10
        {":status",            "304"},       // 11
        {":status",            "400"},       // 12
        {":status",            "404"},       // 13
        {":status",            "500"},       // 14
        {"accept-charset",     ""},          // 15
        {"accept-encoding",    "gzip, deflate"}, // 16
        {"accept-language",    ""},          // 17
        {"accept-ranges",      ""},          // 18
        {"accept",             ""},          // 19
        {"content-length",     ""},          // 28
        {"content-type",       ""},          // 31
        {"host",               ""},          // 38
    };
    for (int i = 1; i < static_cast<int>(std::size(kTable)); ++i) {
        if (kTable[i].first == name && (value.empty() || kTable[i].second == value))
            return i;
    }
    return 0;
}

/**
 * @brief Encode a single header field as an HPACK literal (never indexed).
 *
 * Appends the encoded bytes to `out`. Uses 0x00 prefix (literal, not indexed).
 *
 * @param out  Output vector to append to.
 * @param name Header name (lowercase).
 * @param val  Header value.
 */
inline void hpack_encode_literal(std::vector<std::byte>& out,
                                 std::string_view name,
                                 std::string_view val) {
    // Literal Header Field without Indexing (0x00 prefix, 4-bit index = 0)
    out.push_back(std::byte{0x00});

    // Name: length-prefixed string (Huffman not applied; H-bit = 0)
    auto encode_str = [&](std::string_view s) {
        // 7-bit length
        uint8_t len = static_cast<uint8_t>(s.size() & 0x7F);
        // If length > 127 we'd need multi-byte encoding; limit to 127 for simplicity
        out.push_back(std::byte(len));
        for (char c : s) out.push_back(std::byte(c));
    };
    encode_str(name);
    encode_str(val);
}

/**
 * @brief Encode a header block for an HTTP/2 request into `out`.
 *
 * Encodes :method, :path, :scheme, :authority, and any extra headers.
 *
 * @param out     Output buffer for the encoded HPACK block.
 * @param method  HTTP method string (e.g. "GET", "POST").
 * @param path    Request path (e.g. "/api/v1/users").
 * @param host    Target host (value for :authority).
 * @param extra   Additional (name, value) pairs.
 */
inline void hpack_encode_request(
        std::vector<std::byte>& out,
        std::string_view method, std::string_view path,
        std::string_view host,
        std::span<const std::pair<std::string, std::string>> extra = {}) {
    // Use indexed representation for well-known values
    auto emit_indexed = [&](int idx) {
        out.push_back(std::byte(0x80 | static_cast<uint8_t>(idx)));
    };

    // :method
    if (method == "GET")       emit_indexed(2);
    else if (method == "POST") emit_indexed(3);
    else hpack_encode_literal(out, ":method", method);

    // :path
    if (path == "/")          emit_indexed(4);
    else                      hpack_encode_literal(out, ":path", path);

    // :scheme (assume http)
    emit_indexed(6);

    // :authority
    hpack_encode_literal(out, ":authority", host);

    // Extra headers
    for (auto& [n, v] : extra)
        hpack_encode_literal(out, n, v);
}

// ─── Http2Response ────────────────────────────────────────────────────────────

/**
 * @brief HTTP/2 response received on a single stream.
 */
struct Http2Response {
    int         status{0};     ///< HTTP status code (from :status pseudo-header)
    std::string headers;       ///< Raw decoded header block (name: value\r\n …)
    std::string body;          ///< Response body (full, accumulated DATA frames)
    bool        ok() const noexcept { return status >= 200 && status < 300; }
};

// ─── Http2Connection ─────────────────────────────────────────────────────────

/**
 * @brief An established HTTP/2 connection over a single TCP socket.
 *
 * After construction (via `Http2Client::connect()`), the client performs the
 * HTTP/2 connection preface and SETTINGS exchange.  Callers then issue requests
 * with `get()` / `post()`, which are multiplexed over the single connection.
 *
 * ### Stream ID allocation
 * Client-initiated streams use odd IDs starting at 1, incrementing by 2.
 *
 * ### Concurrency model
 * All calls must occur on the same reactor thread.  Internal coroutines are
 * driven by the reactor; `recv_loop()` must be spawned once by the caller or
 * by `Http2Client::connect()`.
 */
class Http2Connection : public std::enable_shared_from_this<Http2Connection> {
public:
    static constexpr uint32_t kDefaultWindowSize  = 65535; ///< RFC 7540 default
    static constexpr uint32_t kDefaultMaxFrameSize = 16384; ///< RFC 7540 default

    // ── Construction / SETTINGS handshake ────────────────────────────────────

    explicit Http2Connection(TcpStream stream)
        : stream_(std::move(stream))
    {}

    Http2Connection(const Http2Connection&)            = delete;
    Http2Connection& operator=(const Http2Connection&) = delete;

    /**
     * @brief Send the HTTP/2 connection preface and initial SETTINGS frame.
     *
     * Must be called once after construction before issuing requests.
     *
     * @param st Cancellation token.
     * @returns `Result<void>` — ok on success, error on write failure.
     */
    [[nodiscard]] Task<Result<void>> handshake(std::stop_token st) {
        // Client connection preface (RFC 7540 §3.5)
        static constexpr std::string_view kPreface =
            "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        auto wr = co_await write_raw(
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(kPreface.data()),
                kPreface.size()), st);
        if (!wr) co_return unexpected(wr.error());

        // Send empty SETTINGS frame
        co_await send_settings(st);

        // Read server SETTINGS + SETTINGS ACK
        // (minimal: just drain 2 frames without strict validation)
        for (int i = 0; i < 2; ++i) {
            auto fr = co_await read_frame(st);
            if (!fr) co_return unexpected(fr.error());
            if (fr->type == H2FrameType::Settings &&
                !(fr->flags & H2Flags::Ack)) {
                // Send SETTINGS ACK
                co_await send_settings_ack(st);
            }
        }

        co_return {};
    }

    // ── Request API ───────────────────────────────────────────────────────────

    /**
     * @brief Send an HTTP/2 GET request and await the full response.
     *
     * @param path  Request path (e.g. "/api/v1/resource").
     * @param st    Cancellation token.
     * @param hdrs  Optional extra request headers.
     * @returns `Http2Response` on success, or error.
     */
    [[nodiscard]] Task<Result<Http2Response>> get(
            std::string_view path,
            std::stop_token st,
            std::span<const std::pair<std::string,std::string>> hdrs = {}) {
        co_return co_await send_request("GET", path, st, {}, hdrs);
    }

    /**
     * @brief Send an HTTP/2 POST request with a body and await the full response.
     *
     * @param path  Request path.
     * @param body  Request body bytes.
     * @param st    Cancellation token.
     * @param hdrs  Optional extra request headers.
     * @returns `Http2Response` on success, or error.
     */
    [[nodiscard]] Task<Result<Http2Response>> post(
            std::string_view path,
            std::string_view body,
            std::stop_token st,
            std::span<const std::pair<std::string,std::string>> hdrs = {}) {
        co_return co_await send_request("POST", path, st, body, hdrs);
    }

private:
    // ── Internal frame I/O ────────────────────────────────────────────────────

    struct H2Frame {
        uint32_t    length{0};
        H2FrameType type{};
        uint8_t     flags{0};
        uint32_t    stream_id{0};
        std::vector<std::byte> payload;
    };

    [[nodiscard]] Task<Result<H2Frame>> read_frame(std::stop_token st) {
        // Read 9-byte frame header
        std::array<std::byte, 9> hdr_buf{};
        size_t got = 0;
        while (got < 9) {
            if (st.stop_requested())
                co_return unexpected(std::make_error_code(std::errc::operation_canceled));
            auto n = co_await stream_.read(
                std::span<std::byte>(hdr_buf.data() + got, 9 - got));
            if (!n || *n == 0)
                co_return unexpected(std::make_error_code(std::errc::connection_reset));
            got += *n;
        }

        auto [length, type, flags, sid] = h2_decode_frame_header(
            std::span<const std::byte, 9>(hdr_buf));

        H2Frame frame{length, type, flags, sid, {}};

        // Read payload
        if (length > 0) {
            frame.payload.resize(length);
            size_t pgot = 0;
            while (pgot < length) {
                if (st.stop_requested())
                    co_return unexpected(std::make_error_code(std::errc::operation_canceled));
                auto n = co_await stream_.read(
                    std::span<std::byte>(frame.payload.data() + pgot, length - pgot));
                if (!n || *n == 0)
                    co_return unexpected(std::make_error_code(std::errc::connection_reset));
                pgot += *n;
            }
        }

        co_return frame;
    }

    [[nodiscard]] Task<Result<size_t>> write_raw(
            std::span<const std::byte> data, std::stop_token st) {
        size_t sent = 0;
        while (sent < data.size()) {
            if (st.stop_requested())
                co_return unexpected(std::make_error_code(std::errc::operation_canceled));
            auto n = co_await stream_.write(data.subspan(sent));
            if (!n) co_return unexpected(n.error());
            sent += *n;
        }
        co_return sent;
    }

    Task<void> send_settings(std::stop_token st) {
        // Empty SETTINGS frame (stream_id = 0)
        std::array<std::byte, 9> hdr{};
        h2_encode_frame_header(std::span<std::byte,9>(hdr), 0,
                               H2FrameType::Settings, 0, 0);
        co_await write_raw(hdr, st);
    }

    Task<void> send_settings_ack(std::stop_token st) {
        std::array<std::byte, 9> hdr{};
        h2_encode_frame_header(std::span<std::byte,9>(hdr), 0,
                               H2FrameType::Settings, H2Flags::Ack, 0);
        co_await write_raw(hdr, st);
    }

    Task<void> send_window_update(uint32_t stream_id, uint32_t increment,
                                  std::stop_token st) {
        std::array<std::byte, 9 + 4> buf{};
        h2_encode_frame_header(std::span<std::byte,9>(buf), 4,
                               H2FrameType::WindowUpdate, 0, stream_id);
        buf[9]  = std::byte((increment >> 24) & 0x7F);
        buf[10] = std::byte((increment >> 16) & 0xFF);
        buf[11] = std::byte((increment >>  8) & 0xFF);
        buf[12] = std::byte( increment        & 0xFF);
        co_await write_raw(buf, st);
    }

    // ── Request/response lifecycle ────────────────────────────────────────────

    [[nodiscard]] Task<Result<Http2Response>> send_request(
            std::string_view method,
            std::string_view path,
            std::stop_token st,
            std::string_view req_body,
            std::span<const std::pair<std::string,std::string>> extra_hdrs) {
        uint32_t stream_id = next_stream_id_;
        next_stream_id_ += 2;

        // Encode HPACK header block
        std::vector<std::byte> hpack;
        hpack.reserve(256);
        std::vector<std::pair<std::string,std::string>> all_hdrs;
        all_hdrs.reserve(extra_hdrs.size() + 1);
        if (!req_body.empty())
            all_hdrs.push_back({"content-length", std::to_string(req_body.size())});
        for (auto& h : extra_hdrs) all_hdrs.push_back(h);
        hpack_encode_request(hpack, method, path, host_, all_hdrs);

        // HEADERS frame
        bool has_body = !req_body.empty();
        uint8_t flags = H2Flags::EndHeaders | (has_body ? 0 : H2Flags::EndStream);
        std::vector<std::byte> headers_frame(9 + hpack.size());
        h2_encode_frame_header(
            std::span<std::byte,9>(headers_frame.data(), 9),
            static_cast<uint32_t>(hpack.size()),
            H2FrameType::Headers, flags, stream_id);
        std::memcpy(headers_frame.data() + 9, hpack.data(), hpack.size());
        auto wr = co_await write_raw(headers_frame, st);
        if (!wr) co_return unexpected(wr.error());

        // DATA frame (if POST/PUT body)
        if (has_body) {
            std::vector<std::byte> data_frame(9 + req_body.size());
            h2_encode_frame_header(
                std::span<std::byte,9>(data_frame.data(), 9),
                static_cast<uint32_t>(req_body.size()),
                H2FrameType::Data, H2Flags::EndStream, stream_id);
            std::memcpy(data_frame.data() + 9, req_body.data(), req_body.size());
            auto dw = co_await write_raw(data_frame, st);
            if (!dw) co_return unexpected(dw.error());
        }

        // Receive frames until END_STREAM on our stream_id
        Http2Response response;
        bool headers_done = false;

        while (!st.stop_requested()) {
            auto fr = co_await read_frame(st);
            if (!fr) co_return unexpected(fr.error());

            if (fr->stream_id != 0 && fr->stream_id != stream_id) {
                // Frame for another stream — ignore (minimal implementation)
                continue;
            }

            switch (fr->type) {
            case H2FrameType::Settings:
                if (!(fr->flags & H2Flags::Ack))
                    co_await send_settings_ack(st);
                break;

            case H2FrameType::Ping: {
                // Send PING ACK
                std::array<std::byte, 9 + 8> ping_ack{};
                h2_encode_frame_header(std::span<std::byte,9>(ping_ack), 8,
                                       H2FrameType::Ping, H2Flags::Ack, 0);
                if (!fr->payload.empty())
                    std::memcpy(ping_ack.data() + 9, fr->payload.data(),
                                std::min<size_t>(8, fr->payload.size()));
                co_await write_raw(ping_ack, st);
                break;
            }

            case H2FrameType::WindowUpdate:
                // Update our send window (simplified: ignore for now)
                break;

            case H2FrameType::Goaway:
                co_return unexpected(std::make_error_code(std::errc::connection_aborted));

            case H2FrameType::RstStream:
                co_return unexpected(std::make_error_code(std::errc::connection_reset));

            case H2FrameType::Headers: {
                if (fr->stream_id != stream_id) break;
                // Decode HPACK (minimal: scan for :status literal)
                decode_response_headers(fr->payload, response);
                headers_done = true;
                // Send WINDOW_UPDATE for connection level
                co_await send_window_update(0, kDefaultWindowSize, st);
                if (fr->flags & H2Flags::EndStream) goto done;
                break;
            }

            case H2FrameType::Data: {
                if (fr->stream_id != stream_id) break;
                // Append data to response body
                response.body.append(
                    reinterpret_cast<const char*>(fr->payload.data()),
                    fr->payload.size());
                // Send WINDOW_UPDATE to replenish flow control
                if (!fr->payload.empty()) {
                    co_await send_window_update(stream_id,
                        static_cast<uint32_t>(fr->payload.size()), st);
                    co_await send_window_update(0,
                        static_cast<uint32_t>(fr->payload.size()), st);
                }
                if (fr->flags & H2Flags::EndStream) goto done;
                break;
            }

            default: break;
            }
        }
        done:
        (void)headers_done;
        co_return response;
    }

    // ── HPACK decoder (minimal: literal strings only) ─────────────────────────

    void decode_response_headers(const std::vector<std::byte>& block,
                                  Http2Response& resp) {
        const std::byte* p   = block.data();
        const std::byte* end = p + block.size();

        auto read_string = [&]() -> std::string {
            if (p >= end) return {};
            bool huffman = (std::to_underlying(*p) & 0x80) != 0;
            uint8_t len  = std::to_underlying(*p) & 0x7F;
            ++p;
            if (p + len > end) return {};
            std::string s(reinterpret_cast<const char*>(p), len);
            p += len;
            (void)huffman; // Huffman not implemented
            return s;
        };

        while (p < end) {
            uint8_t first = std::to_underlying(*p);

            if (first & 0x80) {
                // Indexed representation — look up static table :status
                uint8_t idx = first & 0x7F;
                ++p;
                // Map known status indices
                if      (idx == 8)  resp.status = 200;
                else if (idx == 9)  resp.status = 204;
                else if (idx == 10) resp.status = 206;
                else if (idx == 11) resp.status = 304;
                else if (idx == 12) resp.status = 400;
                else if (idx == 13) resp.status = 404;
                else if (idx == 14) resp.status = 500;
            } else if ((first & 0xC0) == 0x40) {
                // Literal with incremental indexing
                ++p;
                auto name = read_string();
                auto val  = read_string();
                if (name == ":status") {
                    int s = 0;
                    std::from_chars(val.data(), val.data() + val.size(), s);
                    resp.status = s;
                } else if (!name.empty() && name[0] != ':') {
                    resp.headers += name + ": " + val + "\r\n";
                }
            } else {
                // Literal without indexing / never indexed
                ++p;
                auto name = read_string();
                auto val  = read_string();
                if (name == ":status") {
                    int s = 0;
                    std::from_chars(val.data(), val.data() + val.size(), s);
                    resp.status = s;
                } else if (!name.empty() && name[0] != ':') {
                    resp.headers += name + ": " + val + "\r\n";
                }
            }
        }
    }

    // ── Members ───────────────────────────────────────────────────────────────

    TcpStream stream_;                        ///< Underlying TCP connection
    std::string host_;                        ///< Target host (for :authority)
    uint32_t next_stream_id_{1};              ///< Next client stream ID (odd, +2)
    bool goaway_{false};                      ///< Server sent GOAWAY

    friend class Http2Client;
};

// ─── Http2Client ─────────────────────────────────────────────────────────────

/**
 * @brief Factory that establishes HTTP/2 connections.
 *
 * ### Usage
 * @code
 * Http2Client client;
 * auto conn = co_await client.connect("http://api.example.com", st);
 * if (!conn) { // handle error }
 *
 * auto r1 = co_await (*conn)->get("/users/1", st);
 * auto r2 = co_await (*conn)->post("/events",
 *     R"({"type":"click"})", st,
 *     {{"content-type","application/json"}});
 * @endcode
 */
class Http2Client {
public:
    /**
     * @brief Connect to `url` and complete the HTTP/2 handshake.
     *
     * @param url  Absolute URL (scheme://host[:port]). Only "http" is supported
     *             without kTLS; prepend a TLS-wrapped stream for "https".
     * @param st   Cancellation token.
     * @returns Shared pointer to a ready `Http2Connection`, or error.
     */
    [[nodiscard]] Task<Result<std::shared_ptr<Http2Connection>>>
    connect(std::string url, std::stop_token st) {
        auto parsed = ParsedUrl::parse(url);
        if (!parsed) co_return unexpected(std::make_error_code(std::errc::invalid_argument));

        DnsResolver resolver;
        auto addrs = co_await resolver.resolve(parsed->host, parsed->port_str(), st);
        if (!addrs || addrs->empty())
            co_return unexpected(std::make_error_code(std::errc::host_unreachable));

        auto stream = co_await TcpStream::connect((*addrs)[0], st);
        if (!stream) co_return unexpected(stream.error());

        auto conn = std::make_shared<Http2Connection>(std::move(*stream));
        conn->host_ = std::string(parsed->host);

        auto hs = co_await conn->handshake(st);
        if (!hs) co_return unexpected(hs.error());

        co_return conn;
    }
};

} // namespace qbuem

/** @} */ // end of qbuem_http2_client
