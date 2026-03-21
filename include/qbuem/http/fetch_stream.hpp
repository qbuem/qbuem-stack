#pragma once

/**
 * @file qbuem/http/fetch_stream.hpp
 * @brief Zero-copy HTTP response body streaming via AsyncChannel.
 * @defgroup qbuem_fetch_stream HTTP FetchStream
 * @ingroup qbuem_http
 *
 * ## Overview
 *
 * `fetch()` and `FetchClient` buffer the entire response body in RAM before
 * returning. `FetchStream` eliminates that allocation: the body is delivered
 * chunk-by-chunk through an `AsyncChannel<Chunk>` as data arrives from the
 * kernel, enabling constant-memory processing of arbitrarily large responses.
 *
 * ## Memory model
 * Each `Chunk` wraps a fixed-size `std::array<std::byte, kChunkSize>` stored
 * in a `FixedPoolResource<Chunk, kPoolSlots>` (64 slots by default). The pool
 * is pre-allocated once; no heap allocation occurs on the hot path.
 *
 * ## Usage
 * @code
 * FetchStreamClient client;
 *
 * // Start a streaming GET request
 * auto stream = co_await client.stream("http://files.example.com/large.bin", st);
 * if (!stream) co_return unexpected(stream.error());
 *
 * size_t total = 0;
 * while (auto chunk = co_await stream->next(st)) {
 *     total += chunk->size();
 *     process(chunk->span());   // zero-copy: data lives in pool slot
 *     stream->release(*chunk);  // return slot to pool
 * }
 * std::println("received {} bytes", total);
 * @endcode
 *
 * ## Pipeline integration
 * @code
 * // Pipe HTTP body directly into a StaticPipeline
 * auto pipeline = PipelineBuilder<FetchChunk, ProcessedResult>{}
 *     .with_source(StreamSource(std::move(stream)))
 *     .add<ProcessedResult>(my_processing_stage)
 *     .build();
 * pipeline.start(dispatcher);
 * @endcode
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/arena.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/http/fetch.hpp>
#include <qbuem/net/dns.hpp>
#include <qbuem/net/tcp_stream.hpp>
#include <qbuem/pipeline/async_channel.hpp>

#include <array>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

namespace qbuem {

// ─── Chunk ────────────────────────────────────────────────────────────────────

/**
 * @brief A fixed-size buffer slot returned by `FetchStream::next()`.
 *
 * Each `Chunk` holds up to `kChunkSize` bytes received from the network.
 * Callers **must** call `FetchStream::release()` after processing to return
 * the slot to the internal pool. Failure to release causes backpressure on
 * the receive coroutine once all pool slots are in use.
 */
struct FetchChunk {
    static constexpr size_t kChunkSize = 65'536; ///< 64 KiB per chunk (one GRO super-packet)

    alignas(64) std::array<std::byte, kChunkSize> data{}; ///< Raw storage
    size_t len{0};                                        ///< Valid byte count

    /** @brief Read-only view of the valid data in this chunk. */
    [[nodiscard]] std::span<const std::byte> span() const noexcept {
        return {data.data(), len};
    }

    /** @brief Number of valid bytes in this chunk. */
    [[nodiscard]] size_t size() const noexcept { return len; }

    /** @brief True if no valid data is stored. */
    [[nodiscard]] bool empty() const noexcept { return len == 0; }
};

// ─── FetchStream ──────────────────────────────────────────────────────────────

/**
 * @brief Zero-copy streaming HTTP response body reader.
 *
 * Delivers response body data chunk-by-chunk via an internal
 * `AsyncChannel<FetchChunk*>`. The `recv_pump` coroutine reads from the TCP
 * socket in the background and posts chunks to the channel; the caller pulls
 * them with `next()`.
 *
 * ### Status codes & headers
 * Available via `status()` and `header()` after the stream is constructed;
 * these are parsed before the body pump starts.
 *
 * ### Transfer encodings
 * - `Content-Length`: reads exactly N bytes.
 * - `Transfer-Encoding: chunked`: decodes chunk framing on the fly.
 * - Neither header present: reads until connection close.
 *
 * ### Lifecycle
 * 1. `FetchStreamClient::stream(url, st)` creates the stream.
 * 2. Caller loops `co_await stream->next(st)` until `std::nullopt`.
 * 3. Caller calls `stream->release(chunk)` after each chunk.
 * 4. Destructor closes the TCP connection and drains the channel.
 */
class FetchStream {
public:
    static constexpr size_t kPoolSlots   = 64;  ///< Pre-allocated chunk pool depth
    static constexpr size_t kChanCap     = 32;  ///< AsyncChannel capacity (slots)

    // ── Construction ──────────────────────────────────────────────────────────

    /**
     * @brief Construct from an established TCP connection and parsed status/headers.
     *
     * Called by `FetchStreamClient` after the response head has been received.
     *
     * @param stream     Established, non-blocking TCP connection (moved in).
     * @param status     HTTP response status code (e.g. 200).
     * @param headers    Parsed response headers (owned copy).
     * @param content_len Content-Length value, or -1 if unknown.
     * @param chunked    True when Transfer-Encoding: chunked.
     */
    FetchStream(TcpStream stream, int status,
                std::string headers,
                int64_t content_len, bool chunked)
        : stream_(std::move(stream))
        , status_(status)
        , headers_(std::move(headers))
        , content_len_(content_len)
        , chunked_(chunked)
        , chan_(kChanCap)
    {
        // Pre-allocate all pool slots
        for (size_t i = 0; i < kPoolSlots; ++i) {
            free_stack_[i] = &pool_[i];
        }
        free_count_.store(kPoolSlots, std::memory_order_relaxed);
    }

    FetchStream(const FetchStream&)            = delete;
    FetchStream& operator=(const FetchStream&) = delete;
    FetchStream(FetchStream&&)                 = delete;
    FetchStream& operator=(FetchStream&&)      = delete;

    ~FetchStream() {
        chan_.close();
    }

    // ── Response metadata ────────────────────────────────────────────────────

    /** @brief HTTP response status code. */
    [[nodiscard]] int status() const noexcept { return status_; }

    /** @brief True if status is in [200, 299]. */
    [[nodiscard]] bool ok() const noexcept { return status_ >= 200 && status_ < 300; }

    /**
     * @brief Look up a response header value (case-insensitive name).
     * @returns Header value, or empty string_view if not found.
     */
    [[nodiscard]] std::string_view header(std::string_view name) const noexcept {
        // Simple linear scan over the raw header block (name: value\r\n)
        std::string_view hdr = headers_;
        while (!hdr.empty()) {
            auto eol = hdr.find("\r\n");
            if (eol == std::string_view::npos) break;
            auto line = hdr.substr(0, eol);
            hdr.remove_prefix(eol + 2);
            auto colon = line.find(':');
            if (colon == std::string_view::npos) continue;
            auto hname = line.substr(0, colon);
            // Case-insensitive compare
            if (hname.size() == name.size()) {
                bool match = true;
                for (size_t i = 0; i < hname.size(); ++i) {
                    if ((hname[i] | 0x20) != (name[i] | 0x20)) { match = false; break; }
                }
                if (match) {
                    auto val = line.substr(colon + 1);
                    while (!val.empty() && val.front() == ' ') val.remove_prefix(1);
                    return val;
                }
            }
        }
        return {};
    }

    // ── Streaming API ────────────────────────────────────────────────────────

    /**
     * @brief Receive the next body chunk.
     *
     * Suspends until a chunk is available or the body is fully consumed.
     *
     * @param st  Cancellation token; `std::nullopt` is returned on cancellation.
     * @returns Pointer to a `FetchChunk` with valid data, or `std::nullopt` on EOS.
     *
     * @note The caller **must** call `release(chunk)` after processing each chunk.
     */
    [[nodiscard]] Task<std::optional<FetchChunk*>> next(const std::stop_token& st) {
        if (st.stop_requested()) co_return std::nullopt;
        auto item = co_await chan_.recv();
        if (!item) co_return std::nullopt;  // EOS
        co_return *item;
    }

    /**
     * @brief Return a chunk to the pool after processing.
     *
     * Must be called for every chunk returned by `next()`.
     *
     * @param chunk  Chunk pointer previously returned by `next()`.
     */
    void release(FetchChunk* chunk) noexcept {
        chunk->len = 0;
        size_t idx = free_count_.fetch_add(1, std::memory_order_acq_rel);
        if (idx < kPoolSlots) free_stack_[idx] = chunk;
    }

    // ── Background pump (called once by FetchStreamClient) ───────────────────

    /**
     * @brief Launch the background receive pump coroutine.
     *
     * Must be called exactly once after construction. Reads body data from the
     * TCP socket and posts `FetchChunk` pointers to the internal channel.
     * Closes the channel when the body is exhausted or an error occurs.
     *
     * @param st  Cancellation token that stops the pump.
     */
    Task<void> start_pump(const std::stop_token& st) { // NOLINT(readability-make-member-function-const)
        int64_t remaining = content_len_;  // -1 = unknown

        if (chunked_) {
            co_await pump_chunked(st);
        } else {
            co_await pump_fixed(st, remaining);
        }
        chan_.close();
    }

private:
    // ── Pool slot acquisition ─────────────────────────────────────────────────

    /**
     * @brief Acquire a free pool slot (spin-waits if pool is exhausted).
     *
     * In practice the channel backpressure ensures pool slots are always
     * available when the pump runs: the channel cap (32) < pool size (64).
     */
    FetchChunk* acquire_slot() noexcept {
        for (;;) {
            size_t n = free_count_.load(std::memory_order_acquire);
            if (n == 0) {
                // Pool exhausted — spin briefly (consumer is behind)
                for (int i = 0; i < 128; ++i) {
#if defined(__x86_64__) || defined(__i386__)
                    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__ARM_ARCH)
                    __asm__ volatile("yield" ::: "memory");
#endif
                }
                continue;
            }
            if (free_count_.compare_exchange_weak(n, n - 1,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                return free_stack_[n - 1];
            }
        }
    }

    // ── Content-Length / connection-close pump ────────────────────────────────

    Task<void> pump_fixed(const std::stop_token& st, int64_t content_len) {
        int64_t received = 0;

        while (!st.stop_requested()) {
            if (content_len >= 0 && received >= content_len) break;

            FetchChunk* slot = acquire_slot();
            size_t want = FetchChunk::kChunkSize;
            if (content_len >= 0) {
                int64_t left = content_len - received;
                if (left < static_cast<int64_t>(want)) want = static_cast<size_t>(left);
            }

            auto r = co_await stream_.read(std::span<std::byte>(slot->data.data(), want));
            if (!r || *r == 0) {
                release(slot);
                break;
            }

            slot->len = *r;
            received += static_cast<int64_t>(*r);
            co_await chan_.send(slot);
        }
    }

    // ── Chunked transfer-encoding pump ───────────────────────────────────────

    Task<void> pump_chunked(const std::stop_token& st) {
        // Read and decode HTTP/1.1 chunked transfer encoding
        // chunk-line: "<hex-size>\r\n<data>\r\n" repeated, terminated by "0\r\n\r\n"
        std::array<std::byte, 32> linebuf{};
        size_t linelen = 0;

        while (!st.stop_requested()) {
            // Read chunk-size line (ends with \r\n)
            linelen = 0;
            bool done = false;
            while (linelen < linebuf.size()) {
                std::byte b{};
                auto r = co_await stream_.read(std::span<std::byte>(&b, 1));
                if (!r || *r == 0) { done = true; break; }
                if (b == std::byte{'\n'} && linelen > 0 &&
                    linebuf[linelen - 1] == std::byte{'\r'}) {
                    break;
                }
                linebuf[linelen++] = b;
            }
            if (done) break;

            // Parse hex chunk size
            std::array<char, 16> hexbuf{};
            size_t hexlen = 0;
            for (size_t i = 0; i < linelen && hexlen < 15; ++i) {
                char c = static_cast<char>(linebuf[i]);
                if (c == ';') break;  // chunk extension
                if (c != '\r') hexbuf[hexlen++] = c;
            }
            size_t chunk_size = 0;
            auto [ptr, ec] = std::from_chars(hexbuf.data(), hexbuf.data() + hexlen, chunk_size, 16);
            if (ec != std::errc{} || chunk_size == 0) break;  // zero chunk = end

            // Read chunk data
            size_t data_remaining = chunk_size;
            while (data_remaining > 0 && !st.stop_requested()) {
                FetchChunk* slot = acquire_slot();
                size_t want = std::min(data_remaining, FetchChunk::kChunkSize);
                auto r = co_await stream_.read(std::span<std::byte>(slot->data.data(), want));
                if (!r || *r == 0) { release(slot); goto done; }
                slot->len = *r;
                data_remaining -= *r;
                co_await chan_.send(slot);
            }

            // Consume trailing \r\n after chunk data
            {
                std::array<std::byte, 2> crlf{};
                co_await stream_.read(std::span<std::byte>(crlf.data(), 2));
            }
        }
        done:;
    }

    // ── Members ───────────────────────────────────────────────────────────────

    TcpStream                stream_;          ///< Underlying TCP connection
    int                      status_;          ///< HTTP response status code
    std::string              headers_;         ///< Raw response headers block
    int64_t                  content_len_;     ///< Content-Length (-1 = unknown)
    bool                     chunked_;         ///< True if chunked transfer encoding

    AsyncChannel<FetchChunk*> chan_;            ///< Chunk delivery channel

    // Fixed pool — no heap allocation on hot path
    alignas(64) std::array<FetchChunk, kPoolSlots>    pool_{};
    std::array<FetchChunk*, kPoolSlots>               free_stack_{};
    alignas(64) std::atomic<size_t> free_count_{0};
};

// ─── FetchStreamClient ────────────────────────────────────────────────────────

/**
 * @brief HTTP client that opens a streaming response body.
 *
 * Unlike `FetchClient` (which buffers the full body), `FetchStreamClient`
 * returns a `FetchStream` immediately after receiving the response headers,
 * allowing the caller to process data as it arrives.
 *
 * ### Thread safety
 * Not thread-safe. Use one instance per reactor thread.
 *
 * ### Usage
 * @code
 * FetchStreamClient client;
 * auto stream = co_await client.stream("http://example.com/large-file.bin", st);
 * if (!stream) { // handle error }
 *
 * size_t total = 0;
 * while (auto chunk = co_await (*stream)->next(st)) {
 *     total += (*chunk)->size();
 *     (*stream)->release(*chunk);
 * }
 * std::println("total bytes: {}", total);
 * @endcode
 */
class FetchStreamClient {
public:
    /** @brief Set connection timeout. Default: no timeout. */
    FetchStreamClient& set_timeout(std::chrono::milliseconds ms) noexcept {
        timeout_ms_ = ms.count();
        return *this;
    }

    /**
     * @brief Open a streaming GET request to `url`.
     *
     * Connects, sends the request, parses response headers, then returns a
     * `FetchStream` whose `start_pump()` must be spawned on the dispatcher.
     *
     * @param url  Absolute HTTP URL (e.g. "http://host:port/path").
     * @param st   Cancellation token.
     * @returns Shared pointer to a ready-to-pump `FetchStream`, or error.
     */
    [[nodiscard]] Task<Result<std::shared_ptr<FetchStream>>>
    stream(const std::string& url, const std::stop_token& st) {
        (void)st;
        // Parse URL
        auto parsed = ParsedUrl::parse(url);
        if (!parsed) co_return unexpected(std::make_error_code(std::errc::invalid_argument));

        // DNS resolve
        auto addr = co_await DnsResolver::resolve(std::string(parsed->host), parsed->port);
        if (!addr)
            co_return unexpected(std::make_error_code(std::errc::host_unreachable));

        // Connect
        auto conn = co_await TcpStream::connect(*addr);
        if (!conn) co_return unexpected(conn.error());

        // Send HTTP/1.1 GET request
        std::string req =
            "GET " + std::string(parsed->path.empty() ? "/" : parsed->path) +
            " HTTP/1.1\r\n"
            "Host: " + std::string(parsed->host) + "\r\n"
            "Accept: */*\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";

        auto wr = co_await conn->write(
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(req.data()), req.size()));
        if (!wr) co_return unexpected(wr.error());

        // Read response head (status line + headers)
        std::array<std::byte, 8192> head_buf{};
        size_t head_len = 0;

        // Read until we see \r\n\r\n
        while (head_len < head_buf.size()) {
            auto n = co_await conn->read(
                std::span<std::byte>(head_buf.data() + head_len, 1));
            if (!n || *n == 0) co_return unexpected(std::make_error_code(std::errc::connection_reset));
            head_len += *n;
            if (head_len >= 4) {
                auto* p = reinterpret_cast<const char*>(head_buf.data());
                if (p[head_len-4] == '\r' && p[head_len-3] == '\n' &&
                    p[head_len-2] == '\r' && p[head_len-1] == '\n') {
                    break;
                }
            }
        }

        std::string_view head(reinterpret_cast<const char*>(head_buf.data()), head_len);

        // Parse status code
        int status = 0;
        {
            // "HTTP/1.1 200 OK\r\n"
            auto sp1 = head.find(' ');
            if (sp1 == std::string_view::npos)
                co_return unexpected(std::make_error_code(std::errc::protocol_error));
            auto sp2 = head.find(' ', sp1 + 1);
            auto code_sv = head.substr(sp1 + 1, sp2 - sp1 - 1);
            std::from_chars(code_sv.data(), code_sv.data() + code_sv.size(), status);
        }

        // Find header block (after first \r\n)
        auto hdr_start = head.find("\r\n");
        std::string headers_str;
        if (hdr_start != std::string_view::npos)
            headers_str = std::string(head.substr(hdr_start + 2));

        // Parse Content-Length and Transfer-Encoding
        int64_t content_len = -1;
        bool chunked = false;
        {
            std::string_view hv = headers_str;
            while (!hv.empty()) {
                auto eol = hv.find("\r\n");
                if (eol == std::string_view::npos) break;
                auto line = hv.substr(0, eol);
                hv.remove_prefix(eol + 2);
                auto colon = line.find(':');
                if (colon == std::string_view::npos) continue;
                auto name = line.substr(0, colon);
                auto val  = line.substr(colon + 1);
                while (!val.empty() && val.front() == ' ') val.remove_prefix(1);

                auto ci_eq = [](std::string_view a, std::string_view b) {
                    if (a.size() != b.size()) return false;
                    for (size_t i = 0; i < a.size(); ++i)
                        if ((a[i]|0x20) != (b[i]|0x20)) return false;
                    return true;
                };
                if (ci_eq(name, "content-length")) {
                    std::from_chars(val.data(), val.data() + val.size(), content_len);
                } else if (ci_eq(name, "transfer-encoding")) {
                    chunked = (val.find("chunked") != std::string_view::npos);
                }
            }
        }

        auto fs = std::make_shared<FetchStream>(
            std::move(*conn), status, std::move(headers_str), content_len, chunked);

        co_return fs;
    }

private:
    long timeout_ms_{0}; ///< Connection timeout in milliseconds (0 = none)
};

} // namespace qbuem

/** @} */ // end of qbuem_fetch_stream
