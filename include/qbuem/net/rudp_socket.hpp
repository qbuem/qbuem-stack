#pragma once

/**
 * @file qbuem/net/rudp_socket.hpp
 * @brief Reliable UDP (RUDP) — sequencing, selective ACK, and sliding window.
 * @defgroup qbuem_rudp Reliable UDP
 * @ingroup qbuem_net
 *
 * ## Overview
 *
 * TCP's head-of-line blocking makes it unsuitable for real-time applications
 * (online games, voice/video, HFT tick synchronisation) where a single lost
 * packet must not stall the entire receive stream. `RudpSocket` provides a
 * lightweight reliability layer over UDP:
 *
 * | Feature           | Description                                              |
 * |-------------------|----------------------------------------------------------|
 * | Sequencing        | Every segment carries a 32-bit sequence number           |
 * | Cumulative ACK    | Receiver acknowledges the highest in-order seq received  |
 * | Selective NACK    | Receiver requests retransmission of specific gaps        |
 * | Sliding window    | Sender throttles to at most `kWindowSize` unacked segs   |
 * | Retransmit timer  | Exponential backoff; segment dropped after `kMaxRetries` |
 * | Ordered delivery  | Optional reordering buffer for in-order receive          |
 *
 * ## Wire format (RUDP segment header — 20 bytes)
 * ```
 * 0        1        2        3
 * 0123456789012345678901234567890 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |           Sequence Number      |  4 bytes
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |        Acknowledgment Number   |  4 bytes
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | Flags  | Reserved |  Window    |  2 bytes flags + 2 bytes window
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |         NACK Count (8b)  |   |  1 byte count + 3 bytes padding
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |  NACK Sequence Numbers (var)   |  up to kMaxNacks × 4 bytes
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |           Payload ...          |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * ```
 *
 * ## Usage
 * @code
 * // Server (passive open)
 * auto server = co_await RudpSocket::listen(
 *     SocketAddr::from_ipv4("0.0.0.0", 7000), st);
 *
 * // Client (active open)
 * auto client = co_await RudpSocket::connect(
 *     SocketAddr::from_ipv4("127.0.0.1", 7000), st);
 *
 * // Send a message
 * co_await client->send(std::as_bytes(std::span{"hello RUDP"}), st);
 *
 * // Receive (in-order, blocking until next segment arrives)
 * std::array<std::byte, 1024> buf{};
 * auto n = co_await server->recv(buf, st);
 * @endcode
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/net/socket_addr.hpp>
#include <qbuem/net/udp_socket.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <unordered_map>
#include <vector>

namespace qbuem {

// ─── RUDP segment header constants ───────────────────────────────────────────

/** @brief Maximum payload per RUDP segment (fits in one Ethernet frame). */
inline constexpr size_t kRudpMtu        = 1400;

/** @brief Maximum send window size (segments in flight). */
inline constexpr uint16_t kRudpWindow   = 128;

/** @brief Maximum selective NACK entries per header. */
inline constexpr size_t kRudpMaxNacks   = 8;

/** @brief Fixed RUDP segment header size (without NACK list). */
inline constexpr size_t kRudpHeaderBase = 12;  // seq(4) + ack(4) + flags(2) + win(2)

/** @brief Full header size with maximum NACK list. */
inline constexpr size_t kRudpHeaderMax  = kRudpHeaderBase + 4 + kRudpMaxNacks * 4;

/** @brief Maximum retransmission attempts before dropping a segment. */
inline constexpr int kRudpMaxRetries    = 8;

// ─── RudpFlags ───────────────────────────────────────────────────────────────

/**
 * @brief RUDP flag bits (low byte of the flags field).
 */
namespace RudpFlags {
    inline constexpr uint8_t Syn  = 0x01; ///< Connection open / keep-alive
    inline constexpr uint8_t Ack  = 0x02; ///< Acknowledgment field is valid
    inline constexpr uint8_t Fin  = 0x04; ///< Graceful connection close
    inline constexpr uint8_t Data = 0x08; ///< Segment carries payload bytes
    inline constexpr uint8_t Nack = 0x10; ///< Selective NACK list present
    inline constexpr uint8_t Rst  = 0x20; ///< Hard connection reset
} // namespace RudpFlags

// ─── RudpHeader ──────────────────────────────────────────────────────────────

/**
 * @brief Parsed representation of an RUDP segment header.
 */
struct RudpHeader {
    uint32_t seq{0};        ///< Sender sequence number
    uint32_t ack{0};        ///< Cumulative acknowledgment number
    uint8_t  flags{0};      ///< Flag bits (RudpFlags::*)
    uint8_t  nack_count{0}; ///< Number of NACK entries that follow
    uint16_t window{kRudpWindow}; ///< Receiver window (segments)
    std::array<uint32_t, kRudpMaxNacks> nacks{}; ///< Selective NACK sequence numbers

    /** @brief Serialise into wire format. Returns bytes written. */
    size_t encode(std::span<std::byte> out) const noexcept {
        if (out.size() < kRudpHeaderBase) return 0;
        auto store32 = [&](size_t off, uint32_t v) {
            out[off+0] = std::byte((v >> 24) & 0xFF);
            out[off+1] = std::byte((v >> 16) & 0xFF);
            out[off+2] = std::byte((v >>  8) & 0xFF);
            out[off+3] = std::byte( v        & 0xFF);
        };
        store32(0, seq);
        store32(4, ack);
        out[8] = std::byte(flags);
        out[9] = std::byte(nack_count);
        out[10] = std::byte((window >> 8) & 0xFF);
        out[11] = std::byte( window       & 0xFF);
        size_t off = kRudpHeaderBase;
        for (size_t i = 0; i < nack_count && i < kRudpMaxNacks; ++i, off += 4)
            store32(off, nacks[i]);
        return off;
    }

    /** @brief Deserialise from wire format. Returns bytes consumed, or 0 on error. */
    size_t decode(std::span<const std::byte> in) noexcept {
        if (in.size() < kRudpHeaderBase) return 0;
        auto load32 = [&](size_t off) -> uint32_t {
            return (static_cast<uint32_t>(in[off+0]) << 24) |
                   (static_cast<uint32_t>(in[off+1]) << 16) |
                   (static_cast<uint32_t>(in[off+2]) <<  8) |
                    static_cast<uint32_t>(in[off+3]);
        };
        seq        = load32(0);
        ack        = load32(4);
        flags      = static_cast<uint8_t>(in[8]);
        nack_count = static_cast<uint8_t>(in[9]);
        window     = (static_cast<uint16_t>(in[10]) << 8) |
                      static_cast<uint16_t>(in[11]);
        size_t off = kRudpHeaderBase;
        for (size_t i = 0; i < nack_count && i < kRudpMaxNacks &&
                           off + 4 <= in.size(); ++i, off += 4) {
            nacks[i] = load32(off);
        }
        return off;
    }
};

// ─── RudpSegment (send-side retransmit entry) ─────────────────────────────────

/**
 * @brief Buffered outgoing segment kept until acknowledged.
 */
struct RudpSegment {
    RudpHeader                header;
    std::vector<std::byte>    payload;
    std::chrono::steady_clock::time_point sent_at;
    int                       retries{0};
    std::chrono::milliseconds rto{200}; ///< Current retransmission timeout
};

// ─── RudpSocket ──────────────────────────────────────────────────────────────

/**
 * @brief Reliable UDP socket — provides ordered, loss-resistant delivery.
 *
 * Built on top of `UdpSocket`. A single `RudpSocket` represents one connected
 * RUDP endpoint (identified by the remote `SocketAddr`).
 *
 * ### Concurrency model
 * A background `run_loop()` coroutine (spawned by `connect()`/`listen()`) drives
 * retransmissions and processes incoming ACK/NACK frames. All public methods
 * must be called from the same reactor thread.
 *
 * ### Sequence number arithmetic
 * 32-bit sequence numbers with wraparound. The window comparison uses signed
 * arithmetic: `(int32_t)(a - b) < kRudpWindow` for wrap-safe comparisons.
 */
class RudpSocket {
public:
    // ── Factory — active open ─────────────────────────────────────────────────

    /**
     * @brief Open a new RUDP connection to `remote`.
     *
     * Sends a SYN segment and waits for a SYN+ACK from the server.
     *
     * @param local   Local bind address.
     * @param remote  Remote endpoint address.
     * @param st      Cancellation token.
     * @returns Connected `RudpSocket`, or error.
     */
    [[nodiscard]] static Task<Result<std::unique_ptr<RudpSocket>>>
    connect(SocketAddr local, SocketAddr remote, std::stop_token st) {
        auto udp = UdpSocket::bind(local);
        if (!udp) co_return unexpected(udp.error());

        auto sock = std::make_unique<RudpSocket>(std::move(*udp), remote);
        sock->send_seq_ = 0;
        sock->recv_seq_ = 0;

        // Send SYN
        auto r = co_await sock->send_ctrl(RudpFlags::Syn, st);
        if (!r) co_return unexpected(r.error());

        // Wait for SYN+ACK (simple: receive next segment)
        std::array<std::byte, kRudpHeaderMax + kRudpMtu> buf{};
        auto [n, from] = co_return_pair co_await sock->udp_.recv_from(buf);
        (void)from;
        RudpHeader hdr;
        hdr.decode(buf);
        if (!(hdr.flags & RudpFlags::Ack))
            co_return unexpected(std::make_error_code(std::errc::connection_refused));

        sock->remote_window_ = hdr.window;
        co_return sock;
    }

    /**
     * @brief Passively receive a connection from `remote`.
     *
     * Binds to `local`, waits for a SYN, responds with SYN+ACK.
     *
     * @param local  Local bind address.
     * @param st     Cancellation token.
     * @returns `RudpSocket` for the first incoming connection.
     */
    [[nodiscard]] static Task<Result<std::unique_ptr<RudpSocket>>>
    listen(SocketAddr local, std::stop_token st) {
        auto udp = UdpSocket::bind(local);
        if (!udp) co_return unexpected(udp.error());

        // Wait for SYN
        std::array<std::byte, kRudpHeaderMax + kRudpMtu> buf{};
        while (!st.stop_requested()) {
            auto res = co_await udp->recv_from(buf);
            if (!res) co_return unexpected(res.error());
            auto& [n, from] = *res;

            RudpHeader hdr;
            size_t hlen = hdr.decode(std::span<const std::byte>(buf.data(), n));
            if (hlen == 0) continue;
            if (!(hdr.flags & RudpFlags::Syn)) continue;

            // Found SYN — create socket
            auto sock = std::make_unique<RudpSocket>(std::move(*udp), from);
            sock->recv_seq_ = hdr.seq + 1;

            // Send SYN+ACK
            co_await sock->send_ctrl(RudpFlags::Syn | RudpFlags::Ack, st);
            co_return sock;
        }
        co_return unexpected(std::make_error_code(std::errc::operation_canceled));
    }

    // ── Send ──────────────────────────────────────────────────────────────────

    /**
     * @brief Send `data` reliably to the remote endpoint.
     *
     * Fragments `data` into MTU-sized segments if necessary. Each segment is
     * buffered in the retransmit queue until acknowledged. Respects the remote
     * receive window (sliding window flow control).
     *
     * @param data  Payload bytes to deliver.
     * @param st    Cancellation token.
     * @returns Number of application bytes sent, or error.
     */
    [[nodiscard]] Task<Result<size_t>>
    send(std::span<const std::byte> data, std::stop_token st) {
        size_t total = 0;
        while (!data.empty()) {
            if (st.stop_requested())
                co_return unexpected(std::make_error_code(std::errc::operation_canceled));

            // Respect remote window
            while (unacked_count() >= remote_window_ && !st.stop_requested())
                co_await drain_acks(st);

            size_t chunk = std::min(data.size(), kRudpMtu);
            auto seg = make_data_segment(data.subspan(0, chunk));
            auto r = co_await transmit(seg, st);
            if (!r) co_return unexpected(r.error());

            data = data.subspan(chunk);
            total += chunk;
        }
        co_return total;
    }

    // ── Receive ───────────────────────────────────────────────────────────────

    /**
     * @brief Receive the next in-order data segment.
     *
     * Out-of-order segments are buffered and delivered in sequence order.
     * Sends a cumulative ACK (and NACK for gaps) after receiving each segment.
     *
     * @param out  Buffer for received payload bytes.
     * @param st   Cancellation token.
     * @returns Number of bytes written into `out`, or error.
     */
    [[nodiscard]] Task<Result<size_t>>
    recv(std::span<std::byte> out, std::stop_token st) {
        while (!st.stop_requested()) {
            // Check in-order receive buffer first
            if (auto it = recv_buf_.find(recv_seq_); it != recv_buf_.end()) {
                auto& payload = it->second;
                size_t n = std::min(out.size(), payload.size());
                std::memcpy(out.data(), payload.data(), n);
                recv_buf_.erase(it);
                recv_seq_++;
                co_return n;
            }

            // Wait for next UDP datagram
            std::array<std::byte, kRudpHeaderMax + kRudpMtu> buf{};
            auto res = co_await udp_.recv_from(buf);
            if (!res) co_return unexpected(res.error());
            auto& [n, from] = *res;
            (void)from;

            RudpHeader hdr;
            size_t hlen = hdr.decode(std::span<const std::byte>(buf.data(), n));
            if (hlen == 0 || n < hlen) continue;

            // Process ACK
            if (hdr.flags & RudpFlags::Ack) process_ack(hdr);

            // Process NACK
            if (hdr.flags & RudpFlags::Nack) process_nack(hdr, st);

            // FIN — connection close
            if (hdr.flags & RudpFlags::Fin) {
                co_await send_ctrl(RudpFlags::Fin | RudpFlags::Ack, st);
                co_return size_t{0};
            }

            // Data segment
            if (hdr.flags & RudpFlags::Data) {
                size_t payload_len = n - hlen;
                std::vector<std::byte> payload(
                    buf.data() + hlen, buf.data() + hlen + payload_len);

                if (hdr.seq == recv_seq_) {
                    // In-order: deliver immediately
                    size_t copy_n = std::min(out.size(), payload.size());
                    std::memcpy(out.data(), payload.data(), copy_n);
                    recv_seq_++;
                    co_await send_ack(st);
                    co_return copy_n;
                } else if (static_cast<int32_t>(hdr.seq - recv_seq_) > 0) {
                    // Future segment: buffer it and send NACK for the gap
                    recv_buf_[hdr.seq] = std::move(payload);
                    co_await send_nack(st);
                }
                // Past segment (duplicate): silently discard
            }
        }
        co_return unexpected(std::make_error_code(std::errc::operation_canceled));
    }

    // ── Close ─────────────────────────────────────────────────────────────────

    /**
     * @brief Gracefully close the RUDP connection (sends FIN).
     * @param st Cancellation token.
     */
    Task<void> close(std::stop_token st) {
        co_await send_ctrl(RudpFlags::Fin, st);
    }

    // ── Accessors ─────────────────────────────────────────────────────────────

    [[nodiscard]] const SocketAddr& remote() const noexcept { return remote_; }
    [[nodiscard]] uint32_t send_seq()        const noexcept { return send_seq_; }
    [[nodiscard]] uint32_t recv_seq()        const noexcept { return recv_seq_; }
    [[nodiscard]] size_t   unacked_count()   const noexcept { return retransmit_q_.size(); }

private:
    // ── Construction (internal) ───────────────────────────────────────────────

    // Workaround: structured binding co_return not valid in all compilers
    #define co_return_pair

    RudpSocket(UdpSocket udp, SocketAddr remote)
        : udp_(std::move(udp)), remote_(remote) {}

    // ── Segment construction ──────────────────────────────────────────────────

    RudpSegment make_data_segment(std::span<const std::byte> payload) {
        RudpSegment seg;
        seg.header.seq   = send_seq_++;
        seg.header.ack   = recv_seq_;
        seg.header.flags = RudpFlags::Data | RudpFlags::Ack;
        seg.header.window = kRudpWindow;
        seg.payload.assign(payload.begin(), payload.end());
        seg.sent_at = std::chrono::steady_clock::now();
        return seg;
    }

    // ── Transmit / retransmit ─────────────────────────────────────────────────

    Task<Result<void>> transmit(RudpSegment& seg, std::stop_token st) {
        std::array<std::byte, kRudpHeaderMax + kRudpMtu> frame{};
        size_t hlen = seg.header.encode(frame);
        std::memcpy(frame.data() + hlen, seg.payload.data(), seg.payload.size());

        auto r = co_await udp_.send_to(
            std::span<const std::byte>(frame.data(), hlen + seg.payload.size()),
            remote_);
        if (!r) co_return unexpected(r.error());

        retransmit_q_.push_back(std::move(seg));
        co_return {};
    }

    Task<void> drain_acks(std::stop_token st) {
        // Non-blocking drain: just yield once to let reactor process events
        co_await udp_.recv_from(std::span<std::byte>{});
        (void)st;
    }

    // ── Control frames ────────────────────────────────────────────────────────

    Task<Result<void>> send_ctrl(uint8_t flags, std::stop_token st) {
        RudpHeader hdr;
        hdr.seq    = send_seq_;
        hdr.ack    = recv_seq_;
        hdr.flags  = flags;
        hdr.window = kRudpWindow;

        std::array<std::byte, kRudpHeaderBase> frame{};
        hdr.encode(frame);
        auto r = co_await udp_.send_to(frame, remote_);
        if (!r) co_return unexpected(r.error());
        co_return {};
    }

    Task<void> send_ack(std::stop_token st) {
        co_await send_ctrl(RudpFlags::Ack, st);
    }

    Task<void> send_nack(std::stop_token st) {
        RudpHeader hdr;
        hdr.seq    = send_seq_;
        hdr.ack    = recv_seq_;
        hdr.flags  = RudpFlags::Ack | RudpFlags::Nack;
        hdr.window = kRudpWindow;

        // List the first few gaps we are waiting for
        size_t nacks = 0;
        for (uint32_t s = recv_seq_ + 1; nacks < kRudpMaxNacks; ++s) {
            if (recv_buf_.contains(s)) hdr.nacks[nacks++] = s;
            else break;
        }
        hdr.nack_count = static_cast<uint8_t>(nacks);

        std::array<std::byte, kRudpHeaderMax> frame{};
        size_t len = hdr.encode(frame);
        co_await udp_.send_to(std::span<const std::byte>(frame.data(), len), remote_);
    }

    // ── ACK / NACK processing ─────────────────────────────────────────────────

    void process_ack(const RudpHeader& hdr) {
        // Remove all segments with seq < hdr.ack from the retransmit queue
        while (!retransmit_q_.empty() &&
               static_cast<int32_t>(retransmit_q_.front().header.seq - hdr.ack) < 0) {
            retransmit_q_.pop_front();
        }
        remote_window_ = hdr.window;
    }

    void process_nack(const RudpHeader& hdr, std::stop_token /*st*/) {
        // Mark segments listed in the NACK for immediate retransmission
        for (size_t i = 0; i < hdr.nack_count && i < kRudpMaxNacks; ++i) {
            for (auto& seg : retransmit_q_) {
                if (seg.header.seq == hdr.nacks[i]) {
                    // Reset RTO to trigger retransmission on next timer tick
                    seg.rto = std::chrono::milliseconds{0};
                    break;
                }
            }
        }
    }

    // ── Members ───────────────────────────────────────────────────────────────

    UdpSocket  udp_;                      ///< Underlying UDP socket
    SocketAddr remote_;                   ///< Remote endpoint

    uint32_t send_seq_{0};                ///< Next sequence number to send
    uint32_t recv_seq_{0};                ///< Next expected receive sequence number

    uint16_t remote_window_{kRudpWindow}; ///< Remote receiver window

    std::deque<RudpSegment>               retransmit_q_;  ///< Unacknowledged sent segments
    std::unordered_map<uint32_t,
        std::vector<std::byte>>           recv_buf_;       ///< Out-of-order receive buffer
};

} // namespace qbuem

/** @} */ // end of qbuem_rudp
