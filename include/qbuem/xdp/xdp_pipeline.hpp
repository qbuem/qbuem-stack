#pragma once

/**
 * @file qbuem/xdp/xdp_pipeline.hpp
 * @brief AF_XDP ↔ Pipeline integration — zero-copy packet ingestion source/sink.
 * @defgroup qbuem_xdp_pipeline XDP Pipeline
 * @ingroup qbuem_xdp
 *
 * ## Overview
 *
 * `XdpSource<T>` and `XdpSink<T>` bridge the AF_XDP zero-copy packet path
 * directly into the `StaticPipeline` / `PipelineGraph` framework.
 *
 * Packets arrive from the NIC via DMA directly into UMEM frames — no kernel
 * TCP/IP stack, no extra copies. `XdpSource` pumps those frames into the
 * pipeline as user-defined structs (e.g. `RawPacket`, `EthFrame`).
 *
 * ## Architecture
 * ```
 *  NIC ──DMA──► UMEM frame
 *                    │
 *              XdpSource<T>        // reads XskSocket::recv()
 *                    │ T
 *              Pipeline stage 1    // parse + validate
 *                    │
 *              Pipeline stage N    // application logic
 *                    │
 *              XdpSink<T>          // XskSocket::send() via TX ring
 *                    │
 *              UMEM frame ──DMA──► NIC
 * ```
 *
 * ## Performance targets
 * | Path              | Latency   | Throughput         |
 * |-------------------|-----------|--------------------|
 * | XdpSource recv    | < 500 ns  | > 20 M pkt/s       |
 * | XdpSink send      | < 500 ns  | > 20 M pkt/s       |
 * | End-to-end        | < 1 µs    | > 10 M pkt/s       |
 *
 * ## Usage
 * @code
 * auto umem = xdp::Umem::create({.frame_count = 4096, .use_hugepages = true});
 * auto xsk  = xdp::XskSocket::create("eth0", 0, *umem, {});
 *
 * auto pipeline = PipelineBuilder<RawPacket, ProcessedPacket>{}
 *     .with_source(XdpSource<RawPacket>(xsk.get(), umem.get()))
 *     .add<ParsedPacket>(parse_stage)
 *     .add<ProcessedPacket>(process_stage)
 *     .with_sink(XdpSink<ProcessedPacket>(xsk.get(), umem.get()))
 *     .build();
 *
 * pipeline.start(dispatcher);
 * @endcode
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/xdp/xdp.hpp>

#include <cstdint>
#include <cstring>
#include <span>
#include <stop_token>

namespace qbuem::xdp {

// ─── RawPacket ────────────────────────────────────────────────────────────────

/**
 * @brief Zero-copy packet descriptor passed through the pipeline.
 *
 * Points directly into UMEM memory — no allocation. The pipeline stage
 * receives a view into the DMA'd NIC buffer.
 *
 * @note The UMEM frame must be released back to the Fill Ring after processing
 *       (or recycled via the TX ring for forwarding). Call `release()` if the
 *       packet is consumed without forwarding.
 */
struct RawPacket {
    const std::byte* data{nullptr}; ///< Pointer into UMEM frame data
    uint32_t         len{0};        ///< Packet length in bytes
    uint64_t         addr{0};       ///< UMEM frame descriptor address (for release)
    uint64_t         rx_ns{0};      ///< Hardware RX timestamp (nanoseconds, if available)

    /** @brief View of the raw packet bytes. Zero-copy — no allocation. */
    [[nodiscard]] std::span<const std::byte> span() const noexcept {
        return {data, len};
    }

    /** @brief True if the descriptor is valid. */
    [[nodiscard]] bool valid() const noexcept { return data != nullptr && len > 0; }
};

// ─── XdpSource<T> ─────────────────────────────────────────────────────────────

/**
 * @brief Pipeline source adapter that ingests packets from an AF_XDP socket.
 *
 * Implements the `ISource<T>` protocol required by `PipelineBuilder::with_source()`:
 * - `init(dispatcher, st)` — start the receive pump
 * - `next(st)` → `Task<std::optional<T>>` — deliver the next packet
 *
 * ### Frame lifecycle
 * 1. UMEM Fill Ring supplies empty frames to the kernel.
 * 2. NIC DMA-writes packet into the frame.
 * 3. `XskSocket::recv()` returns the occupied frame descriptor.
 * 4. `XdpSource` wraps it in a `RawPacket` and sends it to the pipeline.
 * 5. After the pipeline has processed the frame, `fill_consumed()` returns
 *    it to the Fill Ring for reuse.
 *
 * @tparam T  Pipeline input type. Must be constructible from `RawPacket`.
 *            Defaults to `RawPacket` for pass-through mode.
 */
template<typename T = RawPacket>
class XdpSource {
public:
    static constexpr size_t kBatchSize = 64; ///< Frames drained per recv() call

    /**
     * @brief Construct from a bound XSK socket and its UMEM.
     *
     * @param xsk   AF_XDP socket (non-owning pointer; outlives this source).
     * @param umem  UMEM backing the socket (non-owning pointer).
     */
    XdpSource(XskSocket* xsk, Umem* umem) noexcept
        : xsk_(xsk), umem_(umem) {}

    /**
     * @brief Initialize the source (pre-fill the Fill Ring).
     */
    void init() noexcept {
        if (umem_) umem_->fill_frames(kBatchSize * 4);
    }

    /**
     * @brief Receive the next packet from the XDP socket.
     *
     * Polls the RX ring. Returns `std::nullopt` when the stop token is triggered.
     *
     * @param st  Cancellation token.
     * @returns Next packet or `std::nullopt` on EOS/cancel.
     */
    [[nodiscard]] Task<std::optional<T>> next(std::stop_token st) {
        while (!st.stop_requested()) {
            if (batch_pos_ >= batch_count_) {
                // Refill batch
                batch_count_ = 0;
                if (xsk_) {
#ifdef QBUEM_HAS_XDP
                    batch_count_ = xsk_->recv(batch_frames_.data(),
                                              static_cast<uint32_t>(kBatchSize));
#endif
                }
                batch_pos_ = 0;
                if (batch_count_ == 0) {
                    // No packets — yield once and retry
                    co_await yield_once();
                    continue;
                }
                // Replenish Fill Ring with consumed frames
                if (umem_) umem_->fill_frames(batch_count_);
            }

            auto& frame = batch_frames_[batch_pos_++];
            RawPacket pkt;
#ifdef QBUEM_HAS_XDP
            if (umem_) pkt.data = reinterpret_cast<const std::byte*>(umem_->data(frame));
            pkt.len  = frame.len;
            pkt.addr = frame.addr;
#endif

            if constexpr (std::is_same_v<T, RawPacket>) {
                co_return pkt;
            } else {
                co_return T{pkt};
            }
        }
        co_return std::nullopt;
    }

private:
    struct YieldAwaiter {
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) { h.resume(); }
        void await_resume() const noexcept {}
    };
    Task<void> yield_once() { co_await YieldAwaiter{}; }

    XskSocket* xsk_;
    Umem*      umem_;
    std::array<UmemFrame, kBatchSize> batch_frames_{};
    uint32_t   batch_count_{0};
    uint32_t   batch_pos_{0};
};

// ─── XdpSink<T> ───────────────────────────────────────────────────────────────

/**
 * @brief Pipeline sink adapter that transmits packets via AF_XDP TX ring.
 *
 * Implements the `ISink<T>` protocol:
 * - `init(dispatcher, st)` — setup
 * - `sink(item, st)` → `Task<Result<void>>` — transmit packet
 *
 * ### Zero-copy transmit
 * The packet data must reside in a UMEM frame. The sink copies the data into
 * a fresh UMEM TX frame if needed, or passes the existing UMEM address directly
 * for true zero-copy forwarding (when the upstream source is also XDP).
 *
 * @tparam T  Pipeline output type. Must expose `.span()` → `std::span<const std::byte>`.
 */
template<typename T = RawPacket>
class XdpSink {
public:
    static constexpr size_t kBatchSize = 64;

    XdpSink(XskSocket* xsk, Umem* umem) noexcept
        : xsk_(xsk), umem_(umem) {}

    /**
     * @brief Transmit a packet via the XDP TX ring.
     *
     * @param item  Pipeline output item with `.span()` payload.
     * @param st    Cancellation token.
     */
    [[nodiscard]] Task<Result<void>>
    sink(T item, std::stop_token st) {
        if (st.stop_requested())
            co_return unexpected(std::make_error_code(std::errc::operation_canceled));

#ifdef QBUEM_HAS_XDP
        if (!xsk_ || !umem_) {
            co_return unexpected(std::make_error_code(std::errc::not_supported));
        }

        auto payload = item.span();
        UmemFrame tx_frame{};
        void* buf = umem_->alloc_tx_frame(tx_frame);
        if (!buf) {
            co_return unexpected(std::make_error_code(std::errc::no_buffer_space));
        }

        size_t copy_len = std::min(payload.size(),
                                   static_cast<size_t>(umem_->frame_size()));
        std::memcpy(buf, payload.data(), copy_len);
        tx_frame.len = static_cast<uint32_t>(copy_len);

        xsk_->send(&tx_frame, 1);
        xsk_->complete_tx();
#endif
        co_return {};
    }

private:
    XskSocket* xsk_;
    Umem*      umem_;
};

} // namespace qbuem::xdp

/** @} */ // end of qbuem_xdp_pipeline
