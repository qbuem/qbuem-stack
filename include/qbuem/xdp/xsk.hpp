#pragma once

/**
 * @file qbuem/xdp/xsk.hpp
 * @brief AF_XDP socket (XSK) abstraction — zero-copy packet send/receive.
 * @ingroup qbuem_xdp
 *
 * `XskSocket` is a RAII wrapper around an AF_XDP socket.
 * Combined with UMEM, it provides zero-copy packet I/O that completely bypasses
 * the kernel network stack between NIC and user space.
 *
 * ### Prerequisites
 * - Linux 4.18+ (`AF_XDP`)
 * - Linux 5.4+ (works without `XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD`)
 * - libbpf enabled: `cmake -DQBUEM_XDP_LIBBPF=ON ..`
 * - CAP_NET_ADMIN or root privileges
 * - XDP support in the NIC driver (native mode: i40e, mlx5, ixgbe, etc.)
 *
 * ### How it works
 * ```
 *  NIC (native XDP)
 *      │ zero-copy DMA
 *      ▼
 *  UMEM (mmap shared memory)
 *      │
 *  ┌───┴──────────────────────┐
 *  │  XskSocket               │
 *  │  Rx Ring → recv()        │
 *  │  Tx Ring ← send()        │
 *  │  Fill Ring → supply empty frames to kernel
 *  │  Completion Ring ← TX complete
 *  └──────────────────────────┘
 * ```
 *
 * ### Usage example
 * @code
 * // 1. Create UMEM
 * auto umem = qbuem::xdp::Umem::create({.frame_count = 4096});
 *
 * // 2. Create XSK socket
 * auto xsk = qbuem::xdp::XskSocket::create("eth0", 0, *umem, {});
 * if (!xsk) { /* handle error */ }
 *
 * // 3. Prepare Fill Ring
 * umem->fill_frames(2048);
 *
 * // 4. Receive loop
 * while (true) {
 *     qbuem::xdp::UmemFrame frames[64];
 *     uint32_t n = xsk->recv(frames, 64);
 *     for (uint32_t i = 0; i < n; ++i) {
 *         auto* pkt = umem->data(frames[i]);
 *         process_packet(pkt, frames[i].len);
 *     }
 *     umem->fill_frames(n); // replenish consumed frames
 * }
 * @endcode
 *
 * @{
 */

#ifdef QBUEM_HAS_XDP

#include <qbuem/xdp/umem.hpp>

#include <cstdint>
#include <string_view>

#if defined(QBUEM_XDP_LIBBPF) || defined(QBUEM_HAS_XDP)
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#ifdef QBUEM_XDP_LIBBPF
#  include <xdp/xsk.h>
#  include <net/if.h>
#endif

namespace qbuem::xdp {

// ─── XskConfig ────────────────────────────────────────────────────────────

/**
 * @brief XSK socket creation configuration.
 */
struct XskConfig {
    /** @brief Rx Ring size (2^n). */
    uint32_t rx_size = kDefaultRingSize;

    /** @brief Tx Ring size (2^n). */
    uint32_t tx_size = kDefaultRingSize;

    /** @brief XDP load mode. */
    enum class Mode : uint8_t {
        Native  = 0, ///< Native mode (XDP hook inside NIC driver, highest performance)
        Skb     = 1, ///< SKB mode (generic fallback, supported by all NICs)
        Offload = 2, ///< Offload mode (XDP executed on NIC hardware)
    } mode = Mode::Native;

    /** @brief Use NEED_WAKEUP flag (Linux 5.3+, reduces unnecessary syscalls). */
    bool need_wakeup = true;

    /** @brief Force zero-copy mode (requires Native mode + driver support). */
    bool force_zerocopy = false;

    /** @brief Shared UMEM (share the same UMEM across multiple queues). */
    bool shared_umem = false;
};

// ─── XskSocket ────────────────────────────────────────────────────────────

/**
 * @brief AF_XDP socket — zero-copy I/O bypassing the kernel network stack.
 *
 * Move-only RAII wrapper. The socket and XDP program are automatically released on destruction.
 *
 * ### Performance characteristics
 * | Mode              | Bandwidth    | CPU usage  | Notes                      |
 * |-------------------|-------------|-----------|----------------------------|
 * | Native XDP        | ~100 Gbps   | Very low   | Requires driver support    |
 * | SKB (generic)     | ~20 Gbps    | Low        | Works on all NICs          |
 * | Offload           | Line rate   | ~0%        | Select SmartNICs only      |
 */
class XskSocket {
public:
    XskSocket(const XskSocket&)            = delete;
    XskSocket& operator=(const XskSocket&) = delete;

    XskSocket(XskSocket&& other) noexcept
#ifdef QBUEM_XDP_LIBBPF
        : xsk_(other.xsk_)
        , rx_(other.rx_)
        , tx_(other.tx_)
        , fd_(other.fd_)
#else
        : fd_(-1)
#endif
    {
#ifdef QBUEM_XDP_LIBBPF
        other.xsk_ = nullptr;
        other.fd_  = -1;
#endif
    }

    ~XskSocket() { destroy(); }

    // ── Factory ──────────────────────────────────────────────────────────

    /**
     * @brief Creates an XSK socket and binds it to an interface/queue.
     *
     * @param ifname   Network interface name (e.g. "eth0").
     * @param queue_id NIC receive queue number (0-based). Multi-queue: one socket per queue.
     * @param umem     UMEM instance to attach (reference; lifetime managed by caller).
     * @param cfg      Socket configuration.
     * @returns XskSocket on success, error_code on failure.
     */
    static Result<XskSocket> create(std::string_view ifname,
                                    uint32_t         queue_id,
                                    Umem&            umem,
                                    const XskConfig& cfg = {}) {
#ifdef QBUEM_XDP_LIBBPF
        XskSocket s;

        xsk_socket_config xsk_cfg{};
        xsk_cfg.rx_size      = cfg.rx_size;
        xsk_cfg.tx_size      = cfg.tx_size;
        xsk_cfg.libbpf_flags = 0;

        switch (cfg.mode) {
            case XskConfig::Mode::Native:
                xsk_cfg.xdp_flags = XDP_FLAGS_DRV_MODE;
                break;
            case XskConfig::Mode::Skb:
                xsk_cfg.xdp_flags = XDP_FLAGS_SKB_MODE;
                break;
            case XskConfig::Mode::Offload:
                xsk_cfg.xdp_flags = XDP_FLAGS_HW_MODE;
                break;
        }
        if (cfg.force_zerocopy) {
            xsk_cfg.bind_flags = XDP_ZEROCOPY;
        } else {
            xsk_cfg.bind_flags = XDP_COPY; // allow copy fallback
        }
        if (cfg.need_wakeup) {
            xsk_cfg.bind_flags |= XDP_USE_NEED_WAKEUP;
        }

        // null-terminate ifname
        char ifbuf[IFNAMSIZ]{};
        ifname.copy(ifbuf, std::min(ifname.size(), size_t{IFNAMSIZ - 1}));

        int ret;
        if (cfg.shared_umem) {
            ret = xsk_socket__create_shared(&s.xsk_, ifbuf, queue_id,
                                            umem.handle(),
                                            &s.rx_, &s.tx_,
                                            umem.fill_ring(), umem.comp_ring(),
                                            &xsk_cfg);
        } else {
            ret = xsk_socket__create(&s.xsk_, ifbuf, queue_id,
                                     umem.handle(),
                                     &s.rx_, &s.tx_,
                                     &xsk_cfg);
        }
        if (ret != 0) {
            return unexpected(std::error_code(-ret, std::system_category()));
        }
        s.fd_ = xsk_socket__fd(s.xsk_);
        return s;
#else
        (void)ifname; (void)queue_id; (void)umem; (void)cfg;
        return unexpected(std::make_error_code(std::errc::not_supported));
#endif
    }

    // ── Receive ──────────────────────────────────────────────────────────

    /**
     * @brief Polls up to `max_n` packets from the Rx Ring (non-blocking).
     *
     * After `recv()`, `Umem::fill_frames()` must be called with the number
     * of consumed frames to replenish the Fill Ring.
     *
     * @param[out] frames  Array to store received frame information.
     * @param      max_n   Maximum number of frames to receive.
     * @returns Actual number of frames received.
     */
    uint32_t recv(UmemFrame* frames, uint32_t max_n) noexcept {
#ifdef QBUEM_XDP_LIBBPF
        uint32_t idx = 0;
        uint32_t rcvd = xsk_ring_cons__peek(&rx_, max_n, &idx);
        for (uint32_t i = 0; i < rcvd; ++i) {
            const xdp_desc* desc = xsk_ring_cons__rx_desc(&rx_, idx++);
            frames[i].addr    = desc->addr;
            frames[i].len     = desc->len;
            frames[i].options = desc->options;
        }
        xsk_ring_cons__release(&rx_, rcvd);
        return rcvd;
#else
        (void)frames; (void)max_n;
        return 0;
#endif
    }

    // ── Transmit ─────────────────────────────────────────────────────────

    /**
     * @brief Queues `n` frames into the Tx Ring and requests transmission from the kernel.
     *
     * @param frames  Array of frames to transmit (addr + len must be set).
     * @param n       Number of frames to transmit.
     * @returns Actual number of frames queued. May be less than n if the Tx Ring is full.
     */
    uint32_t send(const UmemFrame* frames, uint32_t n) noexcept {
#ifdef QBUEM_XDP_LIBBPF
        uint32_t idx = 0;
        uint32_t reserved = xsk_ring_prod__reserve(&tx_, n, &idx);
        for (uint32_t i = 0; i < reserved; ++i) {
            xdp_desc* desc = xsk_ring_prod__tx_desc(&tx_, idx++);
            desc->addr    = frames[i].addr;
            desc->len     = frames[i].len;
            desc->options = frames[i].options;
        }
        xsk_ring_prod__submit(&tx_, reserved);

        // NEED_WAKEUP: call sendto() only when kernel wakeup is needed
        if (xsk_ring_prod__needs_wakeup(&tx_)) {
            ::sendto(fd_, nullptr, 0, MSG_DONTWAIT, nullptr, 0);
        }
        return reserved;
#else
        (void)frames; (void)n;
        return 0;
#endif
    }

    // ── Runtime detection factory ─────────────────────────────────────

    /**
     * @brief Detects at runtime whether AF_XDP is supported on this kernel/platform.
     *
     * Probes by attempting to create `socket(AF_XDP, SOCK_RAW, 0)`.
     * The socket is closed immediately (no side effects).
     *
     * ### Return value
     * - `true`  : AF_XDP socket can be created (Linux 4.18+, AF_XDP enabled)
     * - `false` : Unsupported kernel, missing build flag, or insufficient privileges
     *
     * @code
     * if (!qbuem::xdp::XskSocket::is_supported()) {
     *     // AF_XDP not supported — use UdpSocket fallback
     * }
     * @endcode
     */
    [[nodiscard]] static bool is_supported() noexcept {
#ifdef QBUEM_HAS_XDP
#  ifdef QBUEM_XDP_LIBBPF
      // libbpf build: detect AF_XDP socket directly
      int fd = ::socket(AF_XDP, SOCK_RAW, 0);
      if (fd < 0) return false;
      ::close(fd);
      return true;
#  else
      // stub build: compiled without libbpf, runtime support unavailable
      return false;
#  endif
#else
      return false;
#endif
    }

    // ── Accessors ────────────────────────────────────────────────────────

    /** @brief Socket file descriptor. Used for epoll registration. */
    [[nodiscard]] int fd() const noexcept { return fd_; }

    /** @brief Check whether the socket is valid. */
    [[nodiscard]] bool is_valid() const noexcept { return fd_ >= 0; }

private:
    XskSocket() noexcept
#ifdef QBUEM_XDP_LIBBPF
        : xsk_(nullptr), rx_{}, tx_{}, fd_(-1)
#else
        : fd_(-1)
#endif
    {}

    void destroy() noexcept {
#ifdef QBUEM_XDP_LIBBPF
        if (xsk_) {
            xsk_socket__delete(xsk_);
            xsk_ = nullptr;
            fd_  = -1;
        }
#endif
    }

#ifdef QBUEM_XDP_LIBBPF
    xsk_socket*   xsk_;
    xsk_ring_cons rx_;
    xsk_ring_prod tx_;
#endif
    int fd_;
};

} // namespace qbuem::xdp

#endif // QBUEM_HAS_XDP

/** @} */ // end of qbuem_xdp
