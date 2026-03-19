#pragma once

/**
 * @file qbuem/http/http3_client.hpp
 * @brief HTTP/3 client interface — QUIC-based low-latency transport.
 * @defgroup qbuem_http3_client HTTP/3 Client
 * @ingroup qbuem_http
 *
 * ## Overview
 *
 * HTTP/3 (RFC 9114) runs over QUIC (RFC 9000), a UDP-based transport that
 * eliminates TCP head-of-line blocking and reduces connection setup latency
 * to zero round trips (0-RTT) for returning clients.
 *
 * ## Zero-Dependency Policy
 *
 * qbuem-stack does **not** link any QUIC library directly, following the
 * zero-dependency principle (`include/qbuem/` must compile without third-party
 * headers). Services that need HTTP/3 should:
 *
 * 1. Choose a QUIC library (see table below).
 * 2. Implement `IHttp3Transport` using that library.
 * 3. Inject it into `Http3Client` at construction.
 *
 * | Library     | Language  | Notes                              |
 * |-------------|-----------|------------------------------------|
 * | **quiche**  | Rust/C    | Cloudflare; stable, RFC-compliant  |
 * | **ngtcp2**  | C         | Low-level; full QUIC control       |
 * | **msquic**  | C         | Microsoft; Windows + Linux         |
 * | **mvfst**   | C++       | Meta; Folly-based                  |
 * | **lsquic**  | C         | LiteSpeed; battle-tested           |
 *
 * ## Key HTTP/3 concepts
 *
 * ### QUIC streams
 * - **Bidirectional streams** (IDs 0, 4, 8, …): carry HTTP request/response pairs.
 * - **Unidirectional streams** (IDs 2, 6, 10, …): control, push, QPACK encoder/decoder.
 * - Stream IDs are 62-bit integers allocated by each endpoint.
 *
 * ### QUIC 0-RTT
 * If the client has a cached session ticket, data can be sent in the very first
 * packet without waiting for the server handshake to complete.
 *
 * ### QPACK (RFC 9204)
 * HTTP/3 header compression — similar to HPACK but designed to avoid head-of-line
 * blocking. Uses separate encoder/decoder streams.
 *
 * ### Frame types (HTTP/3, RFC 9114 §7)
 * | Value  | Frame        | Description                          |
 * |--------|-------------|--------------------------------------|
 * | 0x0    | DATA         | Body bytes for a request/response    |
 * | 0x1    | HEADERS      | QPACK-compressed header block        |
 * | 0x3    | CANCEL_PUSH  | Cancel a server push                 |
 * | 0x4    | SETTINGS     | Connection-level settings            |
 * | 0x5    | PUSH_PROMISE | Server push announcement             |
 * | 0x7    | GOAWAY       | Graceful connection close            |
 * | 0xd    | MAX_PUSH_ID  | Limit on server push IDs             |
 *
 * ## Implementation pattern
 *
 * ```cpp
 * // 1. Implement IHttp3Transport using your chosen QUIC library
 * class QuicheHttp3Transport : public qbuem::IHttp3Transport {
 * public:
 *     explicit QuicheHttp3Transport(quiche_h3_conn* conn, quiche_conn* quic_conn)
 *         : conn_(conn), quic_conn_(quic_conn) {}
 *
 *     qbuem::Task<qbuem::Result<qbuem::Http3Response>>
 *     send_request(const qbuem::Http3Request& req,
 *                  std::stop_token st) override {
 *         // Build QPACK headers from req
 *         quiche_h3_header headers[] = { ... };
 *         int64_t stream_id = quiche_h3_send_request(conn_, quic_conn_,
 *                                 headers, nheaders, req.body.empty());
 *         if (!req.body.empty())
 *             quiche_h3_send_body(conn_, quic_conn_, stream_id,
 *                 req.body.data(), req.body.size(), true);
 *         // co_await response frames via UdpSocket event loop
 *         co_return parse_response(stream_id, st);
 *     }
 *
 * private:
 *     quiche_h3_conn* conn_;
 *     quiche_conn*    quic_conn_;
 * };
 *
 * // 2. Inject into Http3Client
 * auto transport = std::make_unique<QuicheHttp3Transport>(h3_conn, quic_conn);
 * qbuem::Http3Client client(std::move(transport));
 *
 * // 3. Issue requests
 * auto resp = co_await client.get("https://api.example.com/v1/status", st);
 * ```
 *
 * ## Usage (with injected transport)
 * @code
 * Http3Client client(std::make_unique<MyQuicTransport>(...));
 * auto resp = co_await client.get("https://api.example.com/data", st);
 * if (resp && resp->ok()) {
 *     std::println("status={} body={}", resp->status, resp->body);
 * }
 * @endcode
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/net/udp_socket.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace qbuem {

// ─── HTTP/3 frame types ───────────────────────────────────────────────────────

/**
 * @brief HTTP/3 frame type values (RFC 9114 §7.2).
 */
enum class H3FrameType : uint64_t {
    Data        = 0x00, ///< Response/request body data
    Headers     = 0x01, ///< QPACK-compressed header block
    CancelPush  = 0x03, ///< Cancel a server push stream
    Settings    = 0x04, ///< Connection settings parameters
    PushPromise = 0x05, ///< Server push announcement
    Goaway      = 0x07, ///< Graceful connection termination
    MaxPushId   = 0x0D, ///< Maximum server push ID
};

/**
 * @brief HTTP/3 SETTINGS parameter identifiers (RFC 9114 §7.2.4).
 */
enum class H3SettingId : uint64_t {
    QpackMaxTableCapacity  = 0x01, ///< Max QPACK dynamic table size
    MaxFieldSectionSize    = 0x06, ///< Max size of compressed header block
    QpackBlockedStreams    = 0x07, ///< Max number of QPACK-blocked streams
};

// ─── HTTP/3 request/response types ───────────────────────────────────────────

/**
 * @brief HTTP/3 request descriptor passed to `IHttp3Transport::send_request()`.
 */
struct Http3Request {
    std::string method;  ///< HTTP method ("GET", "POST", …)
    std::string url;     ///< Full target URL
    std::string body;    ///< Request body (may be empty for GET/HEAD)
    std::vector<std::pair<std::string, std::string>> headers; ///< Extra headers
};

/**
 * @brief HTTP/3 response returned by `IHttp3Transport::send_request()`.
 */
struct Http3Response {
    int         status{0};   ///< HTTP status code
    std::string headers;     ///< Decoded headers (name: value\r\n …)
    std::string body;        ///< Response body
    bool ok() const noexcept { return status >= 200 && status < 300; }
};

// ─── IHttp3Transport — injection interface ────────────────────────────────────

/**
 * @brief Interface that Http3Client delegates all QUIC/HTTP3 I/O through.
 *
 * Implement this interface with your chosen QUIC library (quiche, ngtcp2,
 * msquic, …) and inject it into `Http3Client`.
 *
 * ### Lifecycle
 * - `connect()` is called once to establish the QUIC connection and perform
 *   the TLS + HTTP/3 handshake.
 * - `send_request()` is called for each request.
 * - `close()` is called when the client is destroyed.
 *
 * ### Thread safety
 * Implementations are called from a single reactor thread; no locking required.
 */
class IHttp3Transport {
public:
    virtual ~IHttp3Transport() = default;

    /**
     * @brief Establish the QUIC + HTTP/3 connection to `host:port`.
     *
     * May perform DNS resolution, UDP socket creation, TLS handshake,
     * and the HTTP/3 SETTINGS exchange.
     *
     * @param host  Target hostname (for SNI and DNS resolution).
     * @param port  Target port (typically 443).
     * @param st    Cancellation token.
     * @returns `Result<void>` — ok when the connection is ready.
     */
    [[nodiscard]] virtual Task<Result<void>>
    connect(std::string_view host, uint16_t port, std::stop_token st) = 0;

    /**
     * @brief Send a single HTTP/3 request and return the complete response.
     *
     * @param req  Request descriptor.
     * @param st   Cancellation token.
     * @returns `Http3Response` on success, or error.
     */
    [[nodiscard]] virtual Task<Result<Http3Response>>
    send_request(const Http3Request& req, std::stop_token st) = 0;

    /**
     * @brief Gracefully close the QUIC connection (sends GOAWAY frame).
     *
     * @param st Cancellation token.
     */
    virtual Task<void> close(std::stop_token st) = 0;

    /**
     * @brief Returns true if the connection is still usable.
     */
    [[nodiscard]] virtual bool is_connected() const noexcept = 0;
};

// ─── QUIC variable-length integer encoding (RFC 9000 §16) ────────────────────

/**
 * @brief Encode a QUIC variable-length integer into `out`.
 *
 * @param out   Output buffer (must be 1, 2, 4, or 8 bytes depending on value).
 * @param value Integer to encode (max 2^62 - 1).
 * @returns Number of bytes written.
 */
inline size_t quic_varint_encode(std::byte* out, uint64_t value) noexcept {
    if (value < 64) {                     // 1-byte: 00xxxxxx
        out[0] = std::byte(value & 0x3F);
        return 1;
    } else if (value < 16'384) {          // 2-byte: 01xxxxxx xxxxxxxx
        out[0] = std::byte(0x40 | ((value >> 8) & 0x3F));
        out[1] = std::byte(value & 0xFF);
        return 2;
    } else if (value < 1'073'741'824) {   // 4-byte: 10xxxxxx …
        out[0] = std::byte(0x80 | ((value >> 24) & 0x3F));
        out[1] = std::byte((value >> 16) & 0xFF);
        out[2] = std::byte((value >>  8) & 0xFF);
        out[3] = std::byte( value        & 0xFF);
        return 4;
    } else {                              // 8-byte: 11xxxxxx …
        out[0] = std::byte(0xC0 | ((value >> 56) & 0x3F));
        out[1] = std::byte((value >> 48) & 0xFF);
        out[2] = std::byte((value >> 40) & 0xFF);
        out[3] = std::byte((value >> 32) & 0xFF);
        out[4] = std::byte((value >> 24) & 0xFF);
        out[5] = std::byte((value >> 16) & 0xFF);
        out[6] = std::byte((value >>  8) & 0xFF);
        out[7] = std::byte( value        & 0xFF);
        return 8;
    }
}

/**
 * @brief Decode a QUIC variable-length integer from `in`.
 *
 * @param in     Input byte span.
 * @param[out] consumed  Set to the number of bytes consumed.
 * @returns Decoded value, or 0 on insufficient data.
 */
inline uint64_t quic_varint_decode(std::span<const std::byte> in,
                                    size_t& consumed) noexcept {
    if (in.empty()) { consumed = 0; return 0; }
    uint8_t first = static_cast<uint8_t>(in[0]);
    uint8_t prefix = first >> 6;
    consumed = 1u << prefix;  // 1, 2, 4, or 8
    if (in.size() < consumed) { consumed = 0; return 0; }
    uint64_t val = first & 0x3F;
    for (size_t i = 1; i < consumed; ++i)
        val = (val << 8) | static_cast<uint8_t>(in[i]);
    return val;
}

/**
 * @brief Encode an HTTP/3 frame header (type + length varints) into `out`.
 *
 * @param out    Output buffer (must be at least 16 bytes).
 * @param type   H3 frame type.
 * @param length Payload length.
 * @returns Total bytes written.
 */
inline size_t h3_encode_frame_header(std::byte* out,
                                      H3FrameType type,
                                      uint64_t length) noexcept {
    size_t n = quic_varint_encode(out, std::to_underlying(type));
    n += quic_varint_encode(out + n, length);
    return n;
}

// ─── Http3Client ─────────────────────────────────────────────────────────────

/**
 * @brief HTTP/3 fetch client — delegates I/O to an injected `IHttp3Transport`.
 *
 * Provides the same builder-style API as `FetchClient` and `Http2Client`,
 * hiding the QUIC library details behind the `IHttp3Transport` interface.
 *
 * ### Zero-RTT example (quiche-based)
 * ```cpp
 * // Restore a cached session ticket from disk / SHM
 * auto ticket = load_ticket("api.example.com");
 *
 * auto transport = std::make_unique<QuicheTransport>();
 * transport->set_session_ticket(ticket);  // enables 0-RTT
 *
 * Http3Client client(std::move(transport));
 * // connect() will send SETTINGS + first GET in the same UDP packet (0-RTT)
 * auto conn = co_await client.connect("https://api.example.com", st);
 * auto resp = co_await (*conn)->get("/data", st);
 * ```
 */
class Http3Client {
public:
    /**
     * @brief Construct with an injected QUIC/HTTP3 transport implementation.
     * @param transport  Heap-allocated transport (ownership transferred).
     */
    explicit Http3Client(std::unique_ptr<IHttp3Transport> transport)
        : transport_(std::move(transport)) {}

    Http3Client(const Http3Client&)            = delete;
    Http3Client& operator=(const Http3Client&) = delete;
    Http3Client(Http3Client&&)                 = default;
    Http3Client& operator=(Http3Client&&)      = default;

    /**
     * @brief Connect to `url` and complete the QUIC + HTTP/3 handshake.
     *
     * @param url  Target URL (scheme must be "https" for production use).
     * @param st   Cancellation token.
     * @returns `this` (for chaining), or error.
     */
    [[nodiscard]] Task<Result<Http3Client*>>
    connect(std::string url, std::stop_token st) {
        auto parsed = ParsedUrl::parse(url);
        if (!parsed)
            co_return unexpected(std::make_error_code(std::errc::invalid_argument));

        uint16_t port = 443;
        if (!parsed->port_str().empty()) {
            uint16_t p = 0;
            std::from_chars(parsed->port_str().data(),
                            parsed->port_str().data() + parsed->port_str().size(), p);
            if (p) port = p;
        }

        auto r = co_await transport_->connect(parsed->host, port, st);
        if (!r) co_return unexpected(r.error());
        co_return this;
    }

    /**
     * @brief Issue an HTTP/3 GET request.
     *
     * @param url  Full URL or path.
     * @param st   Cancellation token.
     * @param hdrs Optional extra request headers.
     * @returns `Http3Response` on success, or error.
     */
    [[nodiscard]] Task<Result<Http3Response>> get(
            std::string url, std::stop_token st,
            std::vector<std::pair<std::string,std::string>> hdrs = {}) {
        Http3Request req{.method="GET", .url=std::move(url), .body={}, .headers=std::move(hdrs)};
        co_return co_await transport_->send_request(req, st);
    }

    /**
     * @brief Issue an HTTP/3 POST request with a body.
     *
     * @param url   Full URL or path.
     * @param body  Request body string.
     * @param st    Cancellation token.
     * @param hdrs  Optional extra request headers.
     * @returns `Http3Response` on success, or error.
     */
    [[nodiscard]] Task<Result<Http3Response>> post(
            std::string url, std::string body, std::stop_token st,
            std::vector<std::pair<std::string,std::string>> hdrs = {}) {
        Http3Request req{.method="POST", .url=std::move(url),
                         .body=std::move(body), .headers=std::move(hdrs)};
        co_return co_await transport_->send_request(req, st);
    }

    /**
     * @brief True if the underlying transport is connected.
     */
    [[nodiscard]] bool is_connected() const noexcept {
        return transport_ && transport_->is_connected();
    }

    /**
     * @brief Gracefully close the connection.
     */
    Task<void> close(std::stop_token st) {
        if (transport_) co_await transport_->close(st);
    }

private:
    std::unique_ptr<IHttp3Transport> transport_; ///< Injected QUIC transport
};

} // namespace qbuem

/** @} */ // end of qbuem_http3_client
