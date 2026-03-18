#pragma once

/**
 * @file qbuem/server/http2_handler.hpp
 * @brief HTTP/2 connection handler — frame processing, HPACK, stream state management
 * @defgroup qbuem_http2_handler HTTP/2 Handler
 * @ingroup qbuem_server
 *
 * This header implements the core connection management of the HTTP/2 protocol as a header-only library.
 *
 * ### Key Features
 * - **HTTP/2 frame parsing/serialization**: DATA, HEADERS, SETTINGS, PING, GOAWAY, etc.
 * - **HPACK decoder/encoder**: Header compression based on Static Table (RFC 7541)
 * - **Stream state machine**: IDLE → OPEN → HALF_CLOSED → CLOSED transition management
 * - **Connection preface**: Used after TLS handshake when "h2" ALPN negotiation succeeds
 * - **Coroutine-based async processing**: Uses C++20 Task<> and co_return
 *
 * ### Limitations (minimal implementation)
 * - HPACK dynamic table not supported (only static table 62 entries + literal encoding)
 * - PRIORITY and PUSH_PROMISE frames are silently ignored on receipt
 * - Flow Control not implemented
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/http/request.hpp>
#include <qbuem/http/response.hpp>
#include <qbuem/pipeline/async_channel.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace qbuem {

// ─── Http2FrameType ───────────────────────────────────────────────────────────

/**
 * @brief HTTP/2 frame type enumeration (RFC 7540 section 6).
 *
 * Each value corresponds to the `Type` field (1 byte) of an HTTP/2 frame header.
 */
enum class Http2FrameType : uint8_t {
    DATA          = 0x0, ///< Stream data delivery frame
    HEADERS       = 0x1, ///< Header block + stream priority frame
    PRIORITY      = 0x2, ///< Stream priority assignment frame
    RST_STREAM    = 0x3, ///< Stream forced termination frame
    SETTINGS      = 0x4, ///< Connection settings parameter exchange frame
    PUSH_PROMISE  = 0x5, ///< Server push announcement frame
    PING          = 0x6, ///< Connection liveness check and RTT measurement frame
    GOAWAY        = 0x7, ///< Connection termination notification frame
    WINDOW_UPDATE = 0x8, ///< Flow control window size update frame
    CONTINUATION  = 0x9, ///< HEADERS/PUSH_PROMISE continuation block frame
};

// ─── Http2Frame ───────────────────────────────────────────────────────────────

/**
 * @brief Structure representing a single HTTP/2 frame.
 *
 * Reflects the frame format defined in RFC 7540 section 4.1.
 *
 * ```
 * +-----------------------------------------------+
 * |                 Length (24)                   |
 * +---------------+---------------+---------------+
 * |   Type (8)    |   Flags (8)   |
 * +-+-------------+---------------+-------------------------------+
 * |R|                 Stream Identifier (31)                      |
 * +=+=============================================================+
 * |                   Frame Payload (0...)                        |
 * +---------------------------------------------------------------+
 * ```
 *
 * @note The `length` field is 24-bit but stored as uint32_t (upper 8 bits ignored).
 * @note `stream_id` is 31-bit (MSB is reserved bit, always 0).
 */
struct Http2Frame {
    uint32_t              length{0};    ///< Payload byte count (24-bit valid value)
    Http2FrameType        type{Http2FrameType::DATA}; ///< Frame type
    uint8_t               flags{0};     ///< Type-specific flag bitfield
    uint32_t              stream_id{0}; ///< Stream identifier (31-bit, MSB reserved)
    std::vector<uint8_t>  payload;      ///< Frame payload bytes
};

// ─── HPACK static table flag constants ──────────────────────────────────────

/// @brief END_STREAM flag for HEADERS frames (bit 0)
static constexpr uint8_t HTTP2_FLAG_END_STREAM  = 0x1;
/// @brief END_HEADERS flag for HEADERS/CONTINUATION frames (bit 2)
static constexpr uint8_t HTTP2_FLAG_END_HEADERS = 0x4;
/// @brief ACK flag for SETTINGS frames (bit 0)
static constexpr uint8_t HTTP2_FLAG_ACK         = 0x1;
/// @brief PADDED flag for DATA frames (bit 3)
static constexpr uint8_t HTTP2_FLAG_PADDED      = 0x8;
/// @brief PRIORITY flag for HEADERS frames (bit 5)
static constexpr uint8_t HTTP2_FLAG_PRIORITY    = 0x20;

// ─── HpackDecoder ─────────────────────────────────────────────────────────────

/**
 * @brief HPACK header block decoder (RFC 7541).
 *
 * Minimal implementation supporting only the Static Table (indices 1~61).
 * Dynamic Table is not supported.
 *
 * ### Supported representation formats
 * - **Indexed Header Field** (RFC 7541 section 6.1): bit pattern `1xxxxxxx`
 * - **Literal Header Field — Without Indexing** (RFC 7541 section 6.2.2):
 *   - Name referenced by index: bit pattern `0000xxxx` (upper 4 bits 0)
 *   - Name as literal string: index == 0
 * - **Literal Header Field — Incremental Indexing** (RFC 7541 section 6.2.1):
 *   bit pattern `01xxxxxx` (partially supported; dynamic table updates are ignored)
 *
 * @warning Huffman encoding is not supported. Literal strings are processed as raw bytes only.
 */
class HpackDecoder {
public:
    /**
     * @brief Decodes a header block fragment and returns a header map.
     *
     * @param data HPACK-encoded header block fragment.
     * @returns Map of decoded header name-value pairs.
     *          If multiple headers share the same name, only the last value is preserved.
     */
    std::unordered_map<std::string, std::string> decode(std::span<const uint8_t> data) {
        std::unordered_map<std::string, std::string> headers;
        size_t pos = 0;
        const size_t len = data.size();

        while (pos < len) {
            uint8_t first = data[pos];

            if (first & 0x80) {
                // ── Indexed Header Field representation (RFC 7541 section 6.1) ─
                // Bit pattern: 1xxxxxxx
                // Integer encoding: 7-bit prefix
                uint32_t index = decode_integer(data, pos, 7);
                if (index > 0 && index < 62) {
                    const auto& [name, value] = STATIC_TABLE[index];
                    headers[std::string(name)] = std::string(value);
                }
                // index == 0 is an error; index >= 62 is dynamic table (unsupported) → ignore
            } else if ((first & 0xC0) == 0x40) {
                // ── Literal Header Field — Incremental Indexing (RFC 7541 section 6.2.1) ─
                // Bit pattern: 01xxxxxx
                // Decode name index with 6-bit prefix
                uint32_t name_index = decode_integer(data, pos, 6);
                std::string name;
                if (name_index > 0 && name_index < 62) {
                    name = std::string(STATIC_TABLE[name_index].first);
                } else {
                    name = decode_string(data, pos);
                }
                std::string value = decode_string(data, pos);
                headers[std::move(name)] = std::move(value);
                // Dynamic table update is ignored (minimal implementation)
            } else if ((first & 0xF0) == 0x00) {
                // ── Literal Header Field — Without Indexing (RFC 7541 section 6.2.2) ─
                // Bit pattern: 0000xxxx
                // Decode name index with 4-bit prefix
                uint32_t name_index = decode_integer(data, pos, 4);
                std::string name;
                if (name_index > 0 && name_index < 62) {
                    name = std::string(STATIC_TABLE[name_index].first);
                } else {
                    name = decode_string(data, pos);
                }
                std::string value = decode_string(data, pos);
                headers[std::move(name)] = std::move(value);
            } else if ((first & 0xF0) == 0x10) {
                // ── Literal Header Field — Never Indexed (RFC 7541 section 6.2.3) ─
                // Bit pattern: 0001xxxx
                // Decode name index with 4-bit prefix
                uint32_t name_index = decode_integer(data, pos, 4);
                std::string name;
                if (name_index > 0 && name_index < 62) {
                    name = std::string(STATIC_TABLE[name_index].first);
                } else {
                    name = decode_string(data, pos);
                }
                std::string value = decode_string(data, pos);
                headers[std::move(name)] = std::move(value);
            } else if ((first & 0xE0) == 0x20) {
                // ── Dynamic Table Size Update (RFC 7541 section 6.3) ──────────
                // Bit pattern: 001xxxxx — ignore
                decode_integer(data, pos, 5);
            } else {
                // Unknown representation — skip 1 byte
                ++pos;
            }
        }

        return headers;
    }

private:
    /**
     * @brief Decodes an HPACK integer encoding (RFC 7541 section 5.1).
     *
     * @param data  Source byte span.
     * @param pos   Current read position (in-out). Advances to the next position after decoding.
     * @param prefix_bits Number of prefix bits for the integer (1~8).
     * @returns Decoded integer value.
     */
    static uint32_t decode_integer(std::span<const uint8_t> data,
                                   size_t& pos,
                                   uint8_t prefix_bits) {
        if (pos >= data.size()) return 0;

        const uint8_t mask = static_cast<uint8_t>((1u << prefix_bits) - 1u);
        uint32_t value = data[pos] & mask;
        ++pos;

        if (value < static_cast<uint32_t>(mask)) {
            // Value fits entirely within the prefix
            return value;
        }

        // Multi-byte extension (RFC 7541 section 5.1)
        uint32_t shift = 0;
        while (pos < data.size()) {
            uint8_t byte = data[pos++];
            value += static_cast<uint32_t>(byte & 0x7F) << shift;
            shift += 7;
            if ((byte & 0x80) == 0) break; // MSB == 0: last byte
        }
        return value;
    }

    /**
     * @brief Decodes an HPACK string literal (RFC 7541 section 5.2).
     *
     * Huffman encoding not supported — if the `H` bit is set, returns raw bytes.
     *
     * @param data  Source byte span.
     * @param pos   Current read position (in-out).
     * @returns Decoded string.
     */
    static std::string decode_string(std::span<const uint8_t> data, size_t& pos) {
        if (pos >= data.size()) return {};

        // H bit (most significant bit): indicates Huffman encoding
        // bool huffman = (data[pos] & 0x80) != 0; // currently unsupported
        uint32_t str_len = decode_integer(data, pos, 7);

        if (pos + str_len > data.size()) {
            // Prevent buffer overrun
            str_len = static_cast<uint32_t>(data.size() - pos);
        }

        std::string result(reinterpret_cast<const char*>(data.data() + pos), str_len);
        pos += str_len;
        return result;
    }

    /**
     * @brief HPACK Static Table (RFC 7541 Appendix A).
     *
     * 62 entries corresponding to indices 1~61 (index 0 is unused).
     * Defines pseudo-headers and commonly used general headers including the first 15 entries.
     */
    static const std::pair<std::string_view, std::string_view> STATIC_TABLE[62];
};

// HPACK static table definition (all 61 entries from RFC 7541 Appendix A)
inline const std::pair<std::string_view, std::string_view> HpackDecoder::STATIC_TABLE[62] = {
    // Index 0: unused (RFC 7541 indices start at 1)
    {"", ""},
    // Indices 1~15: pseudo-headers and frequently used headers
    {":authority",                  ""},               //  1
    {":method",                     "GET"},             //  2
    {":method",                     "POST"},            //  3
    {":path",                       "/"},               //  4
    {":path",                       "/index.html"},     //  5
    {":scheme",                     "http"},            //  6
    {":scheme",                     "https"},           //  7
    {":status",                     "200"},             //  8
    {":status",                     "204"},             //  9
    {":status",                     "206"},             // 10
    {":status",                     "304"},             // 11
    {":status",                     "400"},             // 12
    {":status",                     "404"},             // 13
    {":status",                     "500"},             // 14
    {"accept-charset",              ""},               // 15
    // Indices 16~61: general HTTP headers
    {"accept-encoding",             "gzip, deflate"},  // 16
    {"accept-language",             ""},               // 17
    {"accept-ranges",               ""},               // 18
    {"accept",                      ""},               // 19
    {"access-control-allow-origin", ""},               // 20
    {"age",                         ""},               // 21
    {"allow",                       ""},               // 22
    {"authorization",               ""},               // 23
    {"cache-control",               ""},               // 24
    {"content-disposition",         ""},               // 25
    {"content-encoding",            ""},               // 26
    {"content-language",            ""},               // 27
    {"content-length",              ""},               // 28
    {"content-location",            ""},               // 29
    {"content-range",               ""},               // 30
    {"content-type",                ""},               // 31
    {"cookie",                      ""},               // 32
    {"date",                        ""},               // 33
    {"etag",                        ""},               // 34
    {"expect",                      ""},               // 35
    {"expires",                     ""},               // 36
    {"from",                        ""},               // 37
    {"host",                        ""},               // 38
    {"if-match",                    ""},               // 39
    {"if-modified-since",           ""},               // 40
    {"if-none-match",               ""},               // 41
    {"if-range",                    ""},               // 42
    {"if-unmodified-since",         ""},               // 43
    {"last-modified",               ""},               // 44
    {"link",                        ""},               // 45
    {"location",                    ""},               // 46
    {"max-forwards",                ""},               // 47
    {"proxy-authenticate",          ""},               // 48
    {"proxy-authorization",         ""},               // 49
    {"range",                       ""},               // 50
    {"referer",                     ""},               // 51
    {"refresh",                     ""},               // 52
    {"retry-after",                 ""},               // 53
    {"server",                      ""},               // 54
    {"set-cookie",                  ""},               // 55
    {"strict-transport-security",   ""},               // 56
    {"transfer-encoding",           ""},               // 57
    {"user-agent",                  ""},               // 58
    {"vary",                        ""},               // 59
    {"via",                         ""},               // 60
    {"www-authenticate",            ""},               // 61
};

// ─── HpackEncoder ─────────────────────────────────────────────────────────────

/**
 * @brief HPACK header block encoder (RFC 7541).
 *
 * Encodes using a mix of indexed header fields and literal header fields (without indexing).
 * Dynamic table is not used.
 *
 * ### Encoding strategy
 * 1. If both header name+value fully match the static table → indexed representation
 * 2. If only the header name matches the static table → name index + literal value
 * 3. Otherwise → literal name + literal value (without indexing)
 */
class HpackEncoder {
public:
    /**
     * @brief Encodes a header map as an HPACK byte sequence.
     *
     * @param headers Map of header name-value pairs to encode.
     * @returns HPACK-encoded header block byte sequence.
     */
    std::vector<uint8_t> encode(const std::unordered_map<std::string, std::string>& headers) {
        std::vector<uint8_t> result;
        result.reserve(headers.size() * 32); // Estimated average header size

        for (const auto& [name, value] : headers) {
            // ── Step 1: search for exact name+value match ────────────────────
            uint32_t full_match_index = 0;
            uint32_t name_match_index = 0;

            for (uint32_t i = 1; i < 62; ++i) {
                const auto& [tname, tvalue] = HpackDecoder::STATIC_TABLE[i];
                if (tname == name) {
                    if (name_match_index == 0) name_match_index = i;
                    if (tvalue == value) {
                        full_match_index = i;
                        break;
                    }
                }
            }

            if (full_match_index > 0) {
                // ── Indexed Header Field representation (RFC 7541 section 6.1) ─
                // Bit pattern: 1xxxxxxx, 7-bit prefix
                encode_integer(result, full_match_index, 7, 0x80);
            } else if (name_match_index > 0) {
                // ── Literal Header Field — Without Indexing, name index reference ─
                // Bit pattern: 0000xxxx, 4-bit prefix
                encode_integer(result, name_match_index, 4, 0x00);
                encode_string(result, value);
            } else {
                // ── Literal Header Field — Without Indexing, both name+value literal ─
                // Bit pattern: 00000000 (name index = 0)
                result.push_back(0x00);
                encode_string(result, name);
                encode_string(result, value);
            }
        }

        return result;
    }

private:
    /**
     * @brief HPACK integer encoding (RFC 7541 section 5.1).
     *
     * @param out         Output byte vector.
     * @param value       Integer value to encode.
     * @param prefix_bits Number of prefix bits (1~8).
     * @param prefix_byte Upper bit pattern of the prefix byte (bits outside the prefix area).
     */
    static void encode_integer(std::vector<uint8_t>& out,
                                uint32_t value,
                                uint8_t prefix_bits,
                                uint8_t prefix_byte) {
        const uint32_t max_prefix = (1u << prefix_bits) - 1u;

        if (value < max_prefix) {
            out.push_back(static_cast<uint8_t>(prefix_byte | value));
        } else {
            out.push_back(static_cast<uint8_t>(prefix_byte | max_prefix));
            value -= max_prefix;
            while (value >= 128) {
                out.push_back(static_cast<uint8_t>((value & 0x7F) | 0x80));
                value >>= 7;
            }
            out.push_back(static_cast<uint8_t>(value));
        }
    }

    /**
     * @brief HPACK string literal encoding (RFC 7541 section 5.2, Huffman not used).
     *
     * @param out  Output byte vector.
     * @param str  String to encode.
     */
    static void encode_string(std::vector<uint8_t>& out, std::string_view str) {
        // H bit = 0 (Huffman not used), length encoded with 7-bit prefix
        encode_integer(out, static_cast<uint32_t>(str.size()), 7, 0x00);
        out.insert(out.end(),
                   reinterpret_cast<const uint8_t*>(str.data()),
                   reinterpret_cast<const uint8_t*>(str.data()) + str.size());
    }
};

// ─── Http2Stream ──────────────────────────────────────────────────────────────

/**
 * @brief Struct managing the state and data of a single HTTP/2 stream.
 *
 * An HTTP/2 connection multiplexes many streams.
 * Each stream follows the state machine defined in RFC 7540 section 5.1.
 *
 * ### Stream state transitions
 * ```
 *            idle
 *              |
 *         HEADERS received
 *              |
 *            open
 *           /    \
 *  END_STREAM   END_STREAM
 *  (remote)      (local)
 *      |              |
 * half_closed    half_closed
 *  (remote)       (local)
 *      \              /
 *       \            /
 *         closed
 * ```
 */
struct Http2Stream {
    /**
     * @brief Stream state enumeration (RFC 7540 section 5.1).
     */
    enum class State {
        IDLE,              ///< Initial state — stream not yet used
        OPEN,              ///< Bidirectional communication possible
        HALF_CLOSED_LOCAL, ///< Local END_STREAM sent — can only receive from remote
        HALF_CLOSED_REMOTE,///< Remote END_STREAM received — can only send locally
        CLOSED,            ///< Stream fully closed
    };

    uint32_t id{0};                ///< Stream identifier (odd: client-initiated, even: server-initiated)
    State    state{State::IDLE};   ///< Current stream state

    /** @brief Received request header map (HPACK decode result). */
    std::unordered_map<std::string, std::string> request_headers;

    /** @brief Received request body bytes. */
    std::vector<uint8_t> request_body;

    /** @brief Frame queue for frames to send on this stream (outgoing channel). */
    std::shared_ptr<AsyncChannel<Http2Frame>> outgoing;
};

// ─── Http2Handler ─────────────────────────────────────────────────────────────

/**
 * @brief HTTP/2 connection handler (header-only implementation).
 *
 * Core class that handles receiving and sending HTTP/2 frames after
 * TLS-ALPN "h2" negotiation. Operates asynchronously using C++20 coroutines (Task<>).
 *
 * ### Connection processing flow
 * 1. `send_connection_preface()` — Send server connection preface + initial SETTINGS
 * 2. `handle_frame()` — Process each frame received from the network
 *    - SETTINGS: Send ACK response
 *    - PING: Send ACK response
 *    - GOAWAY: Handle connection closure
 *    - HEADERS: HPACK decoding, stream initiation
 *    - DATA: Collect stream body
 *    - END_STREAM: Invoke request handler
 * 3. `send_headers()` / `send_data()` — Send response
 * 4. `drain_pending_frames()` — Collect pending frames to write to socket
 *
 * ### Usage example
 * @code
 * Http2Handler h2([](auto headers, auto body, auto stream) -> Task<void> {
 *     // Process request and send response
 *     co_await h2.send_headers(stream->id,
 *         {{":status", "200"}, {"content-type", "text/plain"}});
 *     co_await h2.send_data(stream->id,
 *         std::span<const uint8_t>(body), true);
 *     co_return;
 * });
 *
 * co_await h2.send_connection_preface();
 * // Network loop:
 * while (true) {
 *     Http2Frame frame = co_await read_frame(socket);
 *     co_await h2.handle_frame(std::move(frame));
 *     for (auto& f : h2.drain_pending_frames())
 *         co_await write_frame(socket, f);
 * }
 * @endcode
 */
class Http2Handler {
public:
    /**
     * @brief Application handler type that processes completed HTTP/2 requests.
     *
     * @param headers  HPACK-decoded request header map.
     * @param body     Request body bytes.
     * @param stream   Stream the request belongs to (used for sending responses).
     */
    using RequestHandler = std::function<Task<void>(
        std::unordered_map<std::string, std::string> headers,
        std::vector<uint8_t> body,
        std::shared_ptr<Http2Stream> stream)>;

    /**
     * @brief Constructs an Http2Handler.
     *
     * @param handler Application handler invoked when a completed HTTP/2 request is received.
     */
    explicit Http2Handler(RequestHandler handler)
        : handler_(std::move(handler)) {}

    // ─── Frame receive processing ─────────────────────────────────────────────

    /**
     * @brief Processes a raw HTTP/2 frame received from the network.
     *
     * Dispatches to the appropriate internal processing method based on frame type.
     * Invokes `handler_` when HEADERS+END_STREAM or DATA+END_STREAM is received.
     *
     * @param frame Received HTTP/2 frame.
     * @returns Processing result. Returns an error code on protocol error.
     */
    Task<Result<void>> handle_frame(Http2Frame frame) {
        switch (frame.type) {
            case Http2FrameType::SETTINGS:
                co_return co_await handle_settings(frame);

            case Http2FrameType::PING:
                co_return co_await handle_ping(frame);

            case Http2FrameType::GOAWAY:
                co_return co_await handle_goaway(frame);

            case Http2FrameType::HEADERS:
                co_return co_await handle_headers(frame);

            case Http2FrameType::CONTINUATION:
                co_return co_await handle_continuation(frame);

            case Http2FrameType::DATA:
                co_return co_await handle_data(frame);

            case Http2FrameType::RST_STREAM:
                co_return co_await handle_rst_stream(frame);

            case Http2FrameType::WINDOW_UPDATE:
                // Flow control not implemented — ignore on receipt
                co_return Result<void>::ok();

            case Http2FrameType::PRIORITY:
                // Priority frame not implemented — ignore on receipt
                co_return Result<void>::ok();

            case Http2FrameType::PUSH_PROMISE:
                // Clients do not send PUSH_PROMISE — ignore
                co_return Result<void>::ok();

            default:
                // Unknown frame type — RFC 7540 section 4.1: ignore
                co_return Result<void>::ok();
        }
    }

    // ─── Response sending ─────────────────────────────────────────────────────

    /**
     * @brief Sends a HEADERS frame on the specified stream.
     *
     * HPACK-encodes the header map and adds a HEADERS frame to `pending_frames_`.
     *
     * @param stream_id  Target stream identifier.
     * @param headers    Map of header name-value pairs to send.
     * @param end_stream Whether to set the END_STREAM flag on the HEADERS frame.
     *                   If true, this frame is the last frame on the stream.
     * @returns Processing result. Returns an error if the stream does not exist.
     */
    Task<Result<void>> send_headers(
            uint32_t stream_id,
            const std::unordered_map<std::string, std::string>& headers,
            bool end_stream = false) {
        std::vector<uint8_t> encoded = hpack_encoder_.encode(headers);

        Http2Frame frame;
        frame.type      = Http2FrameType::HEADERS;
        frame.stream_id = stream_id;
        frame.flags     = HTTP2_FLAG_END_HEADERS;
        if (end_stream) frame.flags |= HTTP2_FLAG_END_STREAM;
        frame.payload   = std::move(encoded);
        frame.length    = static_cast<uint32_t>(frame.payload.size());

        pending_frames_.push_back(std::move(frame));

        // Update stream state
        if (auto it = streams_.find(stream_id); it != streams_.end()) {
            auto& stream = it->second;
            if (end_stream) {
                if (stream->state == Http2Stream::State::OPEN) {
                    stream->state = Http2Stream::State::HALF_CLOSED_LOCAL;
                } else if (stream->state == Http2Stream::State::HALF_CLOSED_REMOTE) {
                    stream->state = Http2Stream::State::CLOSED;
                }
            }
        }

        co_return Result<void>::ok();
    }

    /**
     * @brief Sends a DATA frame on the specified stream.
     *
     * @param stream_id  Target stream identifier.
     * @param data       Byte data to send.
     * @param end_stream Whether to set the END_STREAM flag on the DATA frame.
     *                   If true, this frame is the last data on the stream.
     * @returns Processing result.
     */
    Task<Result<void>> send_data(
            uint32_t stream_id,
            std::span<const uint8_t> data,
            bool end_stream = true) {
        Http2Frame frame;
        frame.type      = Http2FrameType::DATA;
        frame.stream_id = stream_id;
        frame.flags     = end_stream ? HTTP2_FLAG_END_STREAM : 0;
        frame.payload.assign(data.begin(), data.end());
        frame.length    = static_cast<uint32_t>(frame.payload.size());

        pending_frames_.push_back(std::move(frame));

        // Update stream state
        if (end_stream) {
            if (auto it = streams_.find(stream_id); it != streams_.end()) {
                auto& stream = it->second;
                if (stream->state == Http2Stream::State::OPEN) {
                    stream->state = Http2Stream::State::HALF_CLOSED_LOCAL;
                } else if (stream->state == Http2Stream::State::HALF_CLOSED_REMOTE) {
                    stream->state = Http2Stream::State::CLOSED;
                }
            }
        }

        co_return Result<void>::ok();
    }

    // ─── Pending frame collection ─────────────────────────────────────────────

    /**
     * @brief Collects and returns all pending frames to be written to the socket.
     *
     * The internal `pending_frames_` buffer is cleared after returning.
     * The caller must serialize the returned frames and write them to the socket.
     *
     * @returns Vector of frames awaiting transmission. May be empty.
     */
    std::vector<Http2Frame> drain_pending_frames() {
        std::vector<Http2Frame> out;
        out.swap(pending_frames_);
        return out;
    }

    // ─── Connection-level frame sending ──────────────────────────────────────

    /**
     * @brief Adds a SETTINGS frame to `pending_frames_`.
     *
     * @param ack If true, sends an ACK SETTINGS frame (no payload).
     *            If false, sends a SETTINGS frame containing server default parameters.
     */
    Task<void> send_settings(bool ack = false) {
        Http2Frame frame;
        frame.type      = Http2FrameType::SETTINGS;
        frame.stream_id = 0; // Connection-level frames always have stream_id == 0

        if (ack) {
            // ACK SETTINGS: set flag only, no payload
            frame.flags   = HTTP2_FLAG_ACK;
            frame.length  = 0;
        } else {
            // Send server initial SETTINGS parameters
            // Each parameter: 2-byte identifier + 4-byte value = 6 bytes
            frame.flags = 0;
            auto& p = frame.payload;

            // HEADER_TABLE_SIZE (0x1) = 4096
            append_uint16(p, 0x1);
            append_uint32(p, DEFAULT_HEADER_TABLE_SIZE);

            // MAX_FRAME_SIZE (0x5) = 16384
            append_uint16(p, 0x5);
            append_uint32(p, DEFAULT_MAX_FRAME_SIZE);

            // INITIAL_WINDOW_SIZE (0x4) = 65535
            append_uint16(p, 0x4);
            append_uint32(p, DEFAULT_INITIAL_WINDOW_SIZE);

            frame.length = static_cast<uint32_t>(p.size());
        }

        pending_frames_.push_back(std::move(frame));
        co_return;
    }

    /**
     * @brief Adds a PING frame to `pending_frames_`.
     *
     * @param ack          If true, sends an ACK response to a received PING.
     * @param opaque_data  8-byte opaque payload of the PING.
     *                     Must echo the received value back when ACKing.
     */
    Task<void> send_ping(bool ack, uint64_t opaque_data = 0) {
        Http2Frame frame;
        frame.type      = Http2FrameType::PING;
        frame.stream_id = 0;
        frame.flags     = ack ? HTTP2_FLAG_ACK : 0;
        frame.length    = 8; // PING payload is always 8 bytes

        // 8-byte opaque data (big-endian)
        frame.payload.resize(8);
        for (int i = 7; i >= 0; --i) {
            frame.payload[static_cast<size_t>(i)] =
                static_cast<uint8_t>(opaque_data & 0xFF);
            opaque_data >>= 8;
        }

        pending_frames_.push_back(std::move(frame));
        co_return;
    }

    /**
     * @brief Adds a GOAWAY frame to `pending_frames_` and signals connection closure.
     *
     * @param last_stream_id Last stream ID successfully processed by the server.
     * @param error_code     Error code indicating the reason for closure (RFC 7540 section 7).
     * @param message        Optional additional debug message.
     */
    Task<void> send_goaway(uint32_t last_stream_id,
                            uint32_t error_code,
                            std::string_view message = "") {
        Http2Frame frame;
        frame.type      = Http2FrameType::GOAWAY;
        frame.stream_id = 0;
        frame.flags     = 0;

        // Payload: Last-Stream-ID(4 bytes) + Error Code(4 bytes) + additional data
        append_uint32(frame.payload, last_stream_id & 0x7FFFFFFF); // MSB reserved
        append_uint32(frame.payload, error_code);
        if (!message.empty()) {
            frame.payload.insert(frame.payload.end(),
                reinterpret_cast<const uint8_t*>(message.data()),
                reinterpret_cast<const uint8_t*>(message.data()) + message.size());
        }

        frame.length = static_cast<uint32_t>(frame.payload.size());
        pending_frames_.push_back(std::move(frame));
        co_return;
    }

    // ─── Connection preface ───────────────────────────────────────────────────

    /**
     * @brief Sends the HTTP/2 server connection preface.
     *
     * Must be called immediately after TLS handshake and ALPN "h2" negotiation complete.
     * The client sends the "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" preface, and
     * the server responds with an initial SETTINGS frame (RFC 7540 section 3.5).
     *
     * @returns Processing result. Always returns success.
     */
    Task<Result<void>> send_connection_preface() {
        // Server connection preface: send initial SETTINGS frame
        co_await send_settings(false);
        co_return Result<void>::ok();
    }

    // ─── Serialization utilities (static) ────────────────────────────────────

    /**
     * @brief Serializes an HTTP/2 frame as a 9-byte header + payload byte sequence.
     *
     * Follows the frame format of RFC 7540 section 4.1.
     *
     * @param frame Frame to serialize.
     * @returns Serialized byte sequence (9-byte header + payload).
     */
    static std::vector<uint8_t> serialize_frame(const Http2Frame& frame) {
        std::vector<uint8_t> out;
        out.reserve(9 + frame.payload.size());

        // Length (24-bit, big-endian)
        out.push_back(static_cast<uint8_t>((frame.length >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((frame.length >>  8) & 0xFF));
        out.push_back(static_cast<uint8_t>( frame.length        & 0xFF));

        // Type (8-bit)
        out.push_back(static_cast<uint8_t>(frame.type));

        // Flags (8-bit)
        out.push_back(frame.flags);

        // Stream Identifier (31-bit, MSB reserved = 0, big-endian)
        const uint32_t sid = frame.stream_id & 0x7FFFFFFF;
        out.push_back(static_cast<uint8_t>((sid >> 24) & 0xFF));
        out.push_back(static_cast<uint8_t>((sid >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((sid >>  8) & 0xFF));
        out.push_back(static_cast<uint8_t>( sid        & 0xFF));

        // Payload
        out.insert(out.end(), frame.payload.begin(), frame.payload.end());

        return out;
    }

private:
    // ─── Internal frame processing methods ───────────────────────────────────

    /**
     * @brief Processes a SETTINGS frame (RFC 7540 section 6.5).
     *
     * Responds with ACK when a SETTINGS without the ACK flag is received.
     * Ignores ACK SETTINGS.
     *
     * @param frame Received SETTINGS frame.
     * @returns Processing result.
     */
    Task<Result<void>> handle_settings(const Http2Frame& frame) {
        if (frame.stream_id != 0) {
            // SETTINGS must have stream_id == 0 (PROTOCOL_ERROR)
            co_await send_goaway(last_stream_id_, 0x1 /* PROTOCOL_ERROR */,
                                 "SETTINGS with non-zero stream ID");
            co_return Result<void>::err(
                std::make_error_code(std::errc::protocol_error));
        }

        if (frame.flags & HTTP2_FLAG_ACK) {
            // ACK SETTINGS received — ignore
            co_return Result<void>::ok();
        }

        // Process peer SETTINGS parameters (currently ignored, minimal implementation)
        // Send ACK response
        co_await send_settings(true);
        co_return Result<void>::ok();
    }

    /**
     * @brief Processes a PING frame (RFC 7540 section 6.7).
     *
     * Responds with the same opaque_data as ACK when a PING without the ACK flag is received.
     *
     * @param frame Received PING frame.
     * @returns Processing result.
     */
    Task<Result<void>> handle_ping(const Http2Frame& frame) {
        if (frame.flags & HTTP2_FLAG_ACK) {
            // ACK PING received — ignore (RTT measurement etc. not implemented)
            co_return Result<void>::ok();
        }

        // Echo back the received opaque_data
        uint64_t opaque = 0;
        if (frame.payload.size() >= 8) {
            for (int i = 0; i < 8; ++i) {
                opaque = (opaque << 8) | frame.payload[static_cast<size_t>(i)];
            }
        }
        co_await send_ping(true, opaque);
        co_return Result<void>::ok();
    }

    /**
     * @brief Processes a GOAWAY frame (RFC 7540 section 6.8).
     *
     * The peer has requested connection closure; gracefully terminates the connection.
     *
     * @param frame Received GOAWAY frame.
     * @returns Processing result.
     */
    Task<Result<void>> handle_goaway(const Http2Frame& /*frame*/) {
        // Peer GOAWAY received: clean up in-progress streams and close connection
        // Minimal implementation: notify upper layer by returning an error code
        co_return Result<void>::err(
            std::make_error_code(std::errc::connection_aborted));
    }

    /**
     * @brief Processes a HEADERS frame (RFC 7540 section 6.2).
     *
     * Initiates a new stream or processes trailer headers on an existing stream.
     * Completes HPACK decoding when the END_HEADERS flag is set.
     * Invokes the request handler when the END_STREAM flag is set.
     *
     * @param frame Received HEADERS frame.
     * @returns Processing result.
     */
    Task<Result<void>> handle_headers(const Http2Frame& frame) {
        const uint32_t sid = frame.stream_id;
        if (sid == 0) {
            // HEADERS must have stream_id != 0
            co_await send_goaway(last_stream_id_, 0x1 /* PROTOCOL_ERROR */,
                                 "HEADERS with stream_id == 0");
            co_return Result<void>::err(
                std::make_error_code(std::errc::protocol_error));
        }

        // Create or look up stream
        auto& stream = get_or_create_stream(sid);

        // PRIORITY flag: skip 5 bytes at start of payload
        size_t header_block_offset = 0;
        if (frame.flags & HTTP2_FLAG_PRIORITY) {
            header_block_offset = 5;
        }
        // PADDED flag: first byte is the padding length
        uint8_t pad_length = 0;
        if (frame.flags & HTTP2_FLAG_PADDED) {
            if (frame.payload.empty()) {
                co_return Result<void>::err(
                    std::make_error_code(std::errc::protocol_error));
            }
            pad_length = frame.payload[0];
            header_block_offset += 1;
        }

        // Extract header block fragment
        if (header_block_offset > frame.payload.size()) {
            co_return Result<void>::err(
                std::make_error_code(std::errc::protocol_error));
        }
        size_t payload_end = frame.payload.size() - pad_length;
        if (payload_end < header_block_offset) {
            co_return Result<void>::err(
                std::make_error_code(std::errc::protocol_error));
        }

        std::span<const uint8_t> header_block{
            frame.payload.data() + header_block_offset,
            payload_end - header_block_offset
        };

        if (frame.flags & HTTP2_FLAG_END_HEADERS) {
            // Header block complete — HPACK decode
            auto decoded = hpack_decoder_.decode(header_block);
            for (auto& [k, v] : decoded) {
                stream->request_headers[k] = std::move(v);
            }
            stream->state = Http2Stream::State::OPEN;
        } else {
            // Awaiting CONTINUATION frame — store in header block buffer
            // (minimal implementation: continued processing in CONTINUATION frame)
            continuation_buffer_[sid].insert(
                continuation_buffer_[sid].end(),
                header_block.begin(),
                header_block.end());
        }

        if (frame.flags & HTTP2_FLAG_END_STREAM) {
            stream->state = Http2Stream::State::HALF_CLOSED_REMOTE;
            // Invoke handler
            if (handler_) {
                auto t = handler_(stream->request_headers,
                                  stream->request_body,
                                  stream);
                t.detach();
            }
        }

        last_stream_id_ = sid;
        co_return Result<void>::ok();
    }

    /**
     * @brief Processes a CONTINUATION frame (RFC 7540 section 6.10).
     *
     * Receives continuation data of a header block started by a previous
     * HEADERS or PUSH_PROMISE frame.
     *
     * @param frame Received CONTINUATION frame.
     * @returns Processing result.
     */
    Task<Result<void>> handle_continuation(const Http2Frame& frame) {
        const uint32_t sid = frame.stream_id;
        auto it = continuation_buffer_.find(sid);
        if (it == continuation_buffer_.end()) {
            // Unexpected CONTINUATION — protocol error
            co_await send_goaway(last_stream_id_, 0x1 /* PROTOCOL_ERROR */,
                                 "Unexpected CONTINUATION frame");
            co_return Result<void>::err(
                std::make_error_code(std::errc::protocol_error));
        }

        // Append to header block buffer
        auto& buf = it->second;
        buf.insert(buf.end(), frame.payload.begin(), frame.payload.end());

        if (frame.flags & HTTP2_FLAG_END_HEADERS) {
            // Header block complete — HPACK decode
            auto stream_it = streams_.find(sid);
            if (stream_it != streams_.end()) {
                auto decoded = hpack_decoder_.decode(
                    std::span<const uint8_t>(buf.data(), buf.size()));
                for (auto& [k, v] : decoded) {
                    stream_it->second->request_headers[k] = std::move(v);
                }
                stream_it->second->state = Http2Stream::State::OPEN;
            }
            continuation_buffer_.erase(it);
        }

        co_return Result<void>::ok();
    }

    /**
     * @brief Processes a DATA frame (RFC 7540 section 6.1).
     *
     * Collects stream body data.
     * Invokes the request handler when the END_STREAM flag is set.
     *
     * @param frame Received DATA frame.
     * @returns Processing result.
     */
    Task<Result<void>> handle_data(const Http2Frame& frame) {
        const uint32_t sid = frame.stream_id;
        if (sid == 0) {
            // DATA must have stream_id != 0
            co_await send_goaway(last_stream_id_, 0x1 /* PROTOCOL_ERROR */,
                                 "DATA with stream_id == 0");
            co_return Result<void>::err(
                std::make_error_code(std::errc::protocol_error));
        }

        auto it = streams_.find(sid);
        if (it == streams_.end()) {
            // Unknown stream — respond with RST_STREAM
            co_await send_rst_stream(sid, 0x1 /* PROTOCOL_ERROR */);
            co_return Result<void>::ok();
        }

        auto& stream = it->second;

        // Process PADDED flag
        size_t data_offset = 0;
        size_t data_end    = frame.payload.size();
        if (frame.flags & HTTP2_FLAG_PADDED) {
            if (frame.payload.empty()) {
                co_return Result<void>::err(
                    std::make_error_code(std::errc::protocol_error));
            }
            uint8_t pad = frame.payload[0];
            data_offset = 1;
            if (data_end < static_cast<size_t>(pad) + 1) {
                co_return Result<void>::err(
                    std::make_error_code(std::errc::protocol_error));
            }
            data_end -= pad;
        }

        // Collect body data
        stream->request_body.insert(
            stream->request_body.end(),
            frame.payload.begin() + static_cast<ptrdiff_t>(data_offset),
            frame.payload.begin() + static_cast<ptrdiff_t>(data_end));

        if (frame.flags & HTTP2_FLAG_END_STREAM) {
            stream->state = Http2Stream::State::HALF_CLOSED_REMOTE;
            // Invoke handler
            if (handler_) {
                auto t = handler_(stream->request_headers,
                                  stream->request_body,
                                  stream);
                t.detach();
            }
        }

        co_return Result<void>::ok();
    }

    /**
     * @brief Processes an RST_STREAM frame (RFC 7540 section 6.4).
     *
     * Changes the stream state to CLOSED when the peer forcibly closes a stream.
     *
     * @param frame Received RST_STREAM frame.
     * @returns Processing result.
     */
    Task<Result<void>> handle_rst_stream(const Http2Frame& frame) {
        const uint32_t sid = frame.stream_id;
        if (auto it = streams_.find(sid); it != streams_.end()) {
            it->second->state = Http2Stream::State::CLOSED;
        }
        co_return Result<void>::ok();
    }

    /**
     * @brief Sends an RST_STREAM frame.
     *
     * @param stream_id  Stream identifier to close.
     * @param error_code Error code indicating the reason for closure (RFC 7540 section 7).
     */
    Task<void> send_rst_stream(uint32_t stream_id, uint32_t error_code) {
        Http2Frame frame;
        frame.type      = Http2FrameType::RST_STREAM;
        frame.stream_id = stream_id;
        frame.flags     = 0;
        frame.length    = 4;
        append_uint32(frame.payload, error_code);
        pending_frames_.push_back(std::move(frame));
        co_return;
    }

    // ─── Stream management utilities ──────────────────────────────────────────

    /**
     * @brief Looks up a stream by ID or creates a new one.
     *
     * @param sid Stream identifier.
     * @returns shared_ptr reference to the existing or newly created stream.
     */
    std::shared_ptr<Http2Stream>& get_or_create_stream(uint32_t sid) {
        auto it = streams_.find(sid);
        if (it == streams_.end()) {
            auto stream = std::make_shared<Http2Stream>();
            stream->id       = sid;
            stream->state    = Http2Stream::State::IDLE;
            stream->outgoing = std::make_shared<AsyncChannel<Http2Frame>>(64);
            streams_[sid]    = stream;
            return streams_[sid];
        }
        return it->second;
    }

    // ─── Serialization helpers (static) ──────────────────────────────────────

    /**
     * @brief Appends a uint16_t to a vector in big-endian order.
     * @param v   Target vector.
     * @param val 16-bit integer value to append.
     */
    static void append_uint16(std::vector<uint8_t>& v, uint16_t val) {
        v.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        v.push_back(static_cast<uint8_t>( val       & 0xFF));
    }

    /**
     * @brief Appends a uint32_t to a vector in big-endian order.
     * @param v   Target vector.
     * @param val 32-bit integer value to append.
     */
    static void append_uint32(std::vector<uint8_t>& v, uint32_t val) {
        v.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
        v.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
        v.push_back(static_cast<uint8_t>((val >>  8) & 0xFF));
        v.push_back(static_cast<uint8_t>( val        & 0xFF));
    }

    // ─── Member variables ─────────────────────────────────────────────────────

    /** @brief Application handler that processes completed HTTP/2 requests. */
    RequestHandler handler_;

    /** @brief Stream ID → stream object map. */
    std::unordered_map<uint32_t, std::shared_ptr<Http2Stream>> streams_;

    /** @brief HPACK header block decoder instance. */
    HpackDecoder hpack_decoder_;

    /** @brief HPACK header block encoder instance. */
    HpackEncoder hpack_encoder_;

    /** @brief Queue of frames pending write to socket. Collected via `drain_pending_frames()`. */
    std::vector<Http2Frame> pending_frames_;

    /** @brief Last client stream ID processed by the server (used in GOAWAY). */
    uint32_t last_stream_id_{0};

    /**
     * @brief Header block buffer for streams awaiting CONTINUATION frames.
     *
     * Stream ID → incomplete header block byte sequence.
     * Deleted after HPACK decoding upon receiving the END_HEADERS flag.
     */
    std::unordered_map<uint32_t, std::vector<uint8_t>> continuation_buffer_;

    // ─── SETTINGS default value constants ────────────────────────────────────

    /** @brief Default HPACK header table size (RFC 7541 section 6.5.2). */
    static constexpr uint32_t DEFAULT_HEADER_TABLE_SIZE  = 4096;

    /** @brief Default maximum frame size (RFC 7540 section 6.5.2). */
    static constexpr uint32_t DEFAULT_MAX_FRAME_SIZE     = 16384;

    /** @brief Default initial flow control window size (RFC 7540 section 6.5.2). */
    static constexpr uint32_t DEFAULT_INITIAL_WINDOW_SIZE = 65535;
};

} // namespace qbuem

/** @} */ // end of qbuem_http2_handler
