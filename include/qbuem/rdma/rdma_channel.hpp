#pragma once

/**
 * @file qbuem/rdma/rdma_channel.hpp
 * @brief RDMA (RoCE/InfiniBand) cross-host zero-copy messaging.
 * @defgroup qbuem_rdma RDMAChannel
 * @ingroup qbuem_hpc
 *
 * ## Overview
 * Abstracts RDMA Write/Read/Send/Recv through the IBVerbs API.
 * Supports both RoCE v2 (RDMA over Converged Ethernet) and InfiniBand fabrics.
 *
 * ## RDMA operation model
 * ```
 * [Host A]              [IB/RoCE fabric]          [Host B]
 * RDMAChannel::write() ─────────────────────► writes directly to remote memory
 * (no CPU involvement)                         (no CPU involvement on B side)
 *
 * RDMAChannel::send()  ─────────────────────► requires RDMAChannel::recv()
 * (CPU involvement on both sides)
 * ```
 *
 * ## Connection setup (QP handshake)
 * ```
 * A.create_qp() → A.local_info() → [OOB exchange] → A.connect(B.local_info())
 * B.create_qp() → B.local_info() → [OOB exchange] → B.connect(A.local_info())
 * ```
 * OOB (Out-of-Band) exchange is performed via a TCP socket or UDS FD passing.
 *
 * ## Memory registration
 * Buffers must be registered with IBVerbs (`ibv_reg_mr`) before RDMA transfers.
 * `RDMAMr` is a RAII handle for a registered memory region.
 *
 * @code
 * RDMAContext ctx = RDMAContext::open("mlx5_0"); // open RDMA device
 * RDMAChannel ch(ctx);
 * ch.setup(kMaxInflight);
 *
 * auto mr = ctx.register_mr(buf.data(), buf.size());
 * co_await ch.write(remote_addr, remote_key, mr->lkey, buf.data(), buf.size());
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

// IBVerbs header (optional — header is includable without libibverbs)
#if __has_include(<infiniband/verbs.h>)
#  include <infiniband/verbs.h>
#  define QBUEM_HAS_RDMA 1
#endif

namespace qbuem::rdma {

// ─── QP (Queue Pair) information ─────────────────────────────────────────────

/**
 * @brief Local/remote QP connection information.
 *
 * Passed to `connect()` after OOB exchange.
 */
struct QPInfo {
    uint32_t qp_num{0};    ///< QP number
    uint16_t lid{0};       ///< LID (InfiniBand only; 0 for RoCE)
    uint8_t  gid[16]{};    ///< GID (for RoCEv2)
    uint32_t psn{0};       ///< Initial packet sequence number
    uint8_t  port{1};      ///< HCA port number
    uint8_t  gid_index{0}; ///< GID index
};

// ─── Memory registration handle ──────────────────────────────────────────────

/**
 * @brief IBVerbs memory region (MR) RAII handle.
 *
 * Automatically deregistered via `ibv_dereg_mr()` on destruction.
 */
class RDMAMr {
public:
    virtual ~RDMAMr() = default;

    /** @brief Local key (lkey) — used for local send/recv. */
    [[nodiscard]] virtual uint32_t lkey() const noexcept = 0;

    /** @brief Remote key (rkey) — used by the peer for RDMA READ/WRITE. */
    [[nodiscard]] virtual uint32_t rkey() const noexcept = 0;

    /** @brief Start address of the registered buffer. */
    [[nodiscard]] virtual void* addr() const noexcept = 0;

    /** @brief Size of the registered buffer. */
    [[nodiscard]] virtual size_t length() const noexcept = 0;
};

// ─── Completion event ─────────────────────────────────────────────────────────

/**
 * @brief Work Completion (WC) event.
 *
 * Received from CQ (Completion Queue) polling or an event channel.
 */
struct Completion {
    uint64_t wr_id{0};       ///< Work request ID (user-defined)
    uint32_t byte_len{0};    ///< Bytes transferred or received
    uint32_t qp_num{0};      ///< QP number of the completed operation

    enum class Status : uint8_t {
        Success = 0,
        LocalLengthError,
        LocalQpOpError,
        LocalProtError,
        WrFlushError,
        RemoteOpError,
        RemoteInvalidReqError,
        RemoteAccessError,
        Unknown,
    } status{Status::Success};

    enum class Opcode : uint8_t {
        Send     = 0,
        RdmaWrite,
        RdmaRead,
        Recv,
        Unknown,
    } opcode{Opcode::Unknown};

    [[nodiscard]] bool ok() const noexcept { return status == Status::Success; }
};

// ─── RDMAContext ──────────────────────────────────────────────────────────────

/**
 * @brief RDMA HCA (Host Channel Adapter) context.
 *
 * Manages a single RDMA device and its protection domain (PD).
 * Multiple `RDMAChannel` instances may share one `RDMAContext`.
 */
class RDMAContext {
public:
    /**
     * @brief Opens an RDMA device.
     *
     * @param dev_name Device name (e.g. "mlx5_0", "rxe0").
     *                 If empty, the first active device is selected.
     * @returns RDMAContext or error.
     */
    static Result<std::unique_ptr<RDMAContext>> open(
        std::string_view dev_name = "") noexcept;

    RDMAContext() = default;
    RDMAContext(const RDMAContext&) = delete;
    RDMAContext& operator=(const RDMAContext&) = delete;
    virtual ~RDMAContext() = default;

    /**
     * @brief Registers a buffer as an RDMA memory region.
     *
     * @param addr   Start address of the buffer.
     * @param length Buffer size (bytes).
     * @param access Access flags (default: LOCAL_WRITE | REMOTE_WRITE | REMOTE_READ).
     * @returns Memory region handle or error.
     */
    virtual Result<std::unique_ptr<RDMAMr>> register_mr(
        void* addr, size_t length, uint32_t access = 0x7) noexcept = 0;

    /** @brief Device name. */
    [[nodiscard]] virtual std::string_view device_name() const noexcept = 0;

    /** @brief Port LID (InfiniBand only). */
    [[nodiscard]] virtual uint16_t port_lid(uint8_t port = 1) const noexcept = 0;

    /** @brief GID (RoCEv2 only). */
    virtual void gid(uint8_t* out_gid,
                                   uint8_t gid_index = 0) const noexcept = 0;
};

// ─── RDMAChannel ─────────────────────────────────────────────────────────────

/**
 * @brief Bidirectional RDMA channel (RC QP based).
 *
 * ## QP state machine
 * ```
 * RESET → INIT → RTR (Ready To Receive) → RTS (Ready To Send)
 * ```
 * Transitions to RTS via `setup()` → `connect()`.
 *
 * ## In-flight control
 * `max_inflight` limits the number of concurrent outstanding WRs.
 * When exceeded, the caller suspends via `co_await` until a completion is received.
 */
class RDMAChannel {
public:
    explicit RDMAChannel(RDMAContext& ctx) noexcept;
    virtual ~RDMAChannel() = default;

    RDMAChannel(const RDMAChannel&) = delete;
    RDMAChannel& operator=(const RDMAChannel&) = delete;

    // ── QP setup ───────────────────────────────────────────────────────────

    /**
     * @brief Creates a QP and CQ and transitions to the INIT state.
     *
     * @param max_inflight Maximum number of concurrent outstanding WRs.
     * @param port         HCA port number.
     * @returns `Result<void>` on success.
     */
    virtual Result<void> setup(uint32_t max_inflight = 128,
                                uint8_t  port = 1) noexcept = 0;

    /**
     * @brief Returns local QP information (for OOB exchange).
     *
     * @returns QPInfo struct.
     */
    [[nodiscard]] virtual QPInfo local_info() const noexcept = 0;

    /**
     * @brief Connects using remote QP information (RTR → RTS).
     *
     * @param remote QPInfo received from the remote side via OOB.
     * @returns `Result<void>` on success.
     */
    virtual Result<void> connect(const QPInfo& remote) noexcept = 0;

    // ── RDMA transfer operations ───────────────────────────────────────────

    /**
     * @brief RDMA Write — writes directly to remote memory (no remote CPU involvement).
     *
     * @param remote_addr  Virtual address of the remote buffer.
     * @param remote_rkey  Remote memory region rkey.
     * @param local_lkey   Local MR lkey.
     * @param local_buf    Local source buffer.
     * @param len          Number of bytes to transfer.
     * @param wr_id        Work request ID (used to identify the completion event).
     * @returns Completion event.
     */
    virtual Task<Result<Completion>> write(
        uint64_t remote_addr, uint32_t remote_rkey,
        uint32_t local_lkey,  const void* local_buf,
        size_t   len,         uint64_t wr_id = 0) noexcept = 0;

    /**
     * @brief RDMA Read — reads directly from remote memory.
     *
     * @param remote_addr  Virtual address of the remote buffer.
     * @param remote_rkey  Remote MR rkey.
     * @param local_lkey   Local MR lkey.
     * @param local_buf    Local destination buffer.
     * @param len          Number of bytes to read.
     * @param wr_id        Work request ID.
     * @returns Completion event.
     */
    virtual Task<Result<Completion>> read(
        uint64_t remote_addr, uint32_t remote_rkey,
        uint32_t local_lkey,  void*    local_buf,
        size_t   len,         uint64_t wr_id = 0) noexcept = 0;

    /**
     * @brief RDMA Send — operates as a pair with a remote Recv.
     *
     * @param local_lkey Local MR lkey.
     * @param buf        Buffer to send.
     * @param len        Number of bytes to send.
     * @param wr_id      Work request ID.
     */
    virtual Task<Result<Completion>> send(
        uint32_t local_lkey, const void* buf,
        size_t   len,        uint64_t wr_id = 0) noexcept = 0;

    /**
     * @brief Posts a Recv request to the RQ.
     *
     * Must be pre-posted on the remote side before the peer calls `send()`.
     *
     * @param local_lkey Receive buffer MR lkey.
     * @param buf        Receive buffer.
     * @param len        Buffer size.
     * @param wr_id      Work request ID.
     */
    virtual Result<void> post_recv(
        uint32_t local_lkey, void* buf,
        size_t   len,        uint64_t wr_id = 0) noexcept = 0;

    // ── CQ Polling ─────────────────────────────────────────────────────────

    /**
     * @brief Polls the CQ and collects up to `max_wc` completion events.
     *
     * @param out    Output array for completion events.
     * @param max_wc Maximum number of events to collect.
     * @returns Number of completion events collected.
     */
    virtual size_t poll_cq(std::span<Completion> out) noexcept = 0;

    // ── Statistics ─────────────────────────────────────────────────────────

    [[nodiscard]] virtual uint64_t bytes_sent()     const noexcept = 0;
    [[nodiscard]] virtual uint64_t bytes_received() const noexcept = 0;
    [[nodiscard]] virtual uint64_t send_count()     const noexcept = 0;
    [[nodiscard]] virtual uint64_t recv_count()     const noexcept = 0;

protected:
    RDMAContext& ctx_;
};

} // namespace qbuem::rdma

/** @} */
