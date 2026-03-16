#pragma once

/**
 * @file qbuem/rdma/rdma_channel.hpp
 * @brief RDMA (RoCE/InfiniBand) 크로스-호스트 zero-copy 메시징.
 * @defgroup qbuem_rdma RDMAChannel
 * @ingroup qbuem_hpc
 *
 * ## 개요
 * IBVerbs API를 통해 RDMA Write/Read/Send/Recv를 추상화합니다.
 * RoCE v2(RDMA over Converged Ethernet)와 InfiniBand 패브릭 모두 지원합니다.
 *
 * ## RDMA 동작 모델
 * ```
 * [Host A]              [IB/RoCE 패브릭]        [Host B]
 * RDMAChannel::write() ─────────────────────► 원격 메모리에 직접 쓰기
 * (CPU 개입 없음)                              (B 측 CPU 개입 없음)
 *
 * RDMAChannel::send()  ─────────────────────► RDMAChannel::recv() 필요
 * (두 측 CPU 개입)
 * ```
 *
 * ## 연결 설정 (QP 핸드셰이크)
 * ```
 * A.create_qp() → A.local_info() → [OOB 교환] → A.connect(B.local_info())
 * B.create_qp() → B.local_info() → [OOB 교환] → B.connect(A.local_info())
 * ```
 * OOB(Out-of-Band) 교환은 TCP 소켓 또는 UDS FD passing으로 수행합니다.
 *
 * ## 메모리 등록
 * RDMA 전송 전 버퍼를 IBVerbs에 등록(`ibv_reg_mr`)해야 합니다.
 * `RDMAMr`는 등록된 메모리 영역 RAII 핸들입니다.
 *
 * @code
 * RDMAContext ctx = RDMAContext::open("mlx5_0"); // RDMA 디바이스 오픈
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

// IBVerbs 헤더 (선택적 — libibverbs 없어도 헤더 포함 가능)
#if __has_include(<infiniband/verbs.h>)
#  include <infiniband/verbs.h>
#  define QBUEM_HAS_RDMA 1
#endif

namespace qbuem::rdma {

// ─── QP(Queue Pair) 정보 ─────────────────────────────────────────────────────

/**
 * @brief 로컬/원격 QP 연결 정보.
 *
 * OOB 교환 후 `connect()`에 전달합니다.
 */
struct QPInfo {
    uint32_t qp_num{0};    ///< QP 번호
    uint16_t lid{0};       ///< LID (InfiniBand only, RoCE는 0)
    uint8_t  gid[16]{};    ///< GID (RoCEv2 용)
    uint32_t psn{0};       ///< 초기 패킷 시퀀스 번호
    uint8_t  port{1};      ///< HCA 포트 번호
    uint8_t  gid_index{0}; ///< GID 인덱스
};

// ─── 메모리 등록 핸들 ────────────────────────────────────────────────────────

/**
 * @brief IBVerbs 메모리 영역(MR) RAII 핸들.
 *
 * 소멸 시 `ibv_dereg_mr()`로 자동 해제됩니다.
 */
class RDMAMr {
public:
    virtual ~RDMAMr() = default;

    /** @brief 로컬 키 (lkey) — 로컬 send/recv에 사용. */
    [[nodiscard]] virtual uint32_t lkey() const noexcept = 0;

    /** @brief 원격 키 (rkey) — 상대방이 RDMA READ/WRITE 시 사용. */
    [[nodiscard]] virtual uint32_t rkey() const noexcept = 0;

    /** @brief 등록된 버퍼 시작 주소. */
    [[nodiscard]] virtual void* addr() const noexcept = 0;

    /** @brief 등록된 버퍼 크기. */
    [[nodiscard]] virtual size_t length() const noexcept = 0;
};

// ─── 완료 이벤트 ─────────────────────────────────────────────────────────────

/**
 * @brief Work Completion (WC) 이벤트.
 *
 * CQ(Completion Queue) Polling 또는 이벤트 채널에서 수신합니다.
 */
struct Completion {
    uint64_t wr_id{0};       ///< 작업 요청 ID (사용자 정의)
    uint32_t byte_len{0};    ///< 전송/수신된 바이트 수
    uint32_t qp_num{0};      ///< 완료된 QP 번호

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
 * @brief RDMA HCA(Host Channel Adapter) 컨텍스트.
 *
 * 단일 RDMA 디바이스와 보호 도메인(PD)을 관리합니다.
 * 여러 `RDMAChannel`이 하나의 `RDMAContext`를 공유할 수 있습니다.
 */
class RDMAContext {
public:
    /**
     * @brief RDMA 디바이스를 엽니다.
     *
     * @param dev_name 디바이스 이름 (e.g. "mlx5_0", "rxe0").
     *                 빈 문자열이면 첫 번째 활성 디바이스를 선택합니다.
     * @returns RDMAContext 또는 에러.
     */
    static Result<std::unique_ptr<RDMAContext>> open(
        std::string_view dev_name = "") noexcept;

    RDMAContext() = default;
    RDMAContext(const RDMAContext&) = delete;
    RDMAContext& operator=(const RDMAContext&) = delete;
    virtual ~RDMAContext() = default;

    /**
     * @brief 버퍼를 RDMA 메모리 영역으로 등록합니다.
     *
     * @param addr   버퍼 시작 주소.
     * @param length 버퍼 크기 (bytes).
     * @param access 접근 플래그 (기본: LOCAL_WRITE | REMOTE_WRITE | REMOTE_READ).
     * @returns 메모리 영역 핸들 또는 에러.
     */
    virtual Result<std::unique_ptr<RDMAMr>> register_mr(
        void* addr, size_t length, uint32_t access = 0x7) noexcept = 0;

    /** @brief 디바이스 이름. */
    [[nodiscard]] virtual std::string_view device_name() const noexcept = 0;

    /** @brief 포트 LID (InfiniBand 전용). */
    [[nodiscard]] virtual uint16_t port_lid(uint8_t port = 1) const noexcept = 0;

    /** @brief GID (RoCEv2 전용). */
    virtual void gid(uint8_t* out_gid,
                                   uint8_t gid_index = 0) const noexcept = 0;
};

// ─── RDMAChannel ─────────────────────────────────────────────────────────────

/**
 * @brief RDMA 양방향 채널 (RC QP 기반).
 *
 * ## QP 상태 머신
 * ```
 * RESET → INIT → RTR(Ready To Receive) → RTS(Ready To Send)
 * ```
 * `setup()` → `connect()`를 통해 RTS 상태로 전환합니다.
 *
 * ## In-flight 제어
 * `max_inflight`는 동시 outstanding WR 수를 제한합니다.
 * 초과 시 `co_await`으로 완료 대기합니다.
 */
class RDMAChannel {
public:
    explicit RDMAChannel(RDMAContext& ctx) noexcept;
    virtual ~RDMAChannel() = default;

    RDMAChannel(const RDMAChannel&) = delete;
    RDMAChannel& operator=(const RDMAChannel&) = delete;

    // ── QP 설정 ────────────────────────────────────────────────────────────

    /**
     * @brief QP와 CQ를 생성하고 INIT 상태로 전환합니다.
     *
     * @param max_inflight 최대 동시 outstanding WR 수.
     * @param port         HCA 포트 번호.
     * @returns 성공 시 `Result<void>`.
     */
    virtual Result<void> setup(uint32_t max_inflight = 128,
                                uint8_t  port = 1) noexcept = 0;

    /**
     * @brief 로컬 QP 정보를 반환합니다 (OOB 교환용).
     *
     * @returns QPInfo 구조체.
     */
    [[nodiscard]] virtual QPInfo local_info() const noexcept = 0;

    /**
     * @brief 원격 QP 정보를 사용하여 연결합니다 (RTR → RTS).
     *
     * @param remote 원격 측에서 OOB로 수신한 QPInfo.
     * @returns 성공 시 `Result<void>`.
     */
    virtual Result<void> connect(const QPInfo& remote) noexcept = 0;

    // ── RDMA 전송 작업 ─────────────────────────────────────────────────────

    /**
     * @brief RDMA Write — 원격 메모리에 직접 씁니다 (원격 CPU 개입 없음).
     *
     * @param remote_addr  원격 버퍼 가상 주소.
     * @param remote_rkey  원격 메모리 영역 rkey.
     * @param local_lkey   로컬 MR lkey.
     * @param local_buf    로컬 소스 버퍼.
     * @param len          전송 바이트 수.
     * @param wr_id        작업 ID (완료 이벤트에서 식별용).
     * @returns 완료 이벤트.
     */
    virtual Task<Result<Completion>> write(
        uint64_t remote_addr, uint32_t remote_rkey,
        uint32_t local_lkey,  const void* local_buf,
        size_t   len,         uint64_t wr_id = 0) noexcept = 0;

    /**
     * @brief RDMA Read — 원격 메모리에서 직접 읽습니다.
     *
     * @param remote_addr  원격 버퍼 가상 주소.
     * @param remote_rkey  원격 MR rkey.
     * @param local_lkey   로컬 MR lkey.
     * @param local_buf    로컬 대상 버퍼.
     * @param len          읽을 바이트 수.
     * @param wr_id        작업 ID.
     * @returns 완료 이벤트.
     */
    virtual Task<Result<Completion>> read(
        uint64_t remote_addr, uint32_t remote_rkey,
        uint32_t local_lkey,  void*    local_buf,
        size_t   len,         uint64_t wr_id = 0) noexcept = 0;

    /**
     * @brief RDMA Send — 원격 Recv와 쌍으로 동작합니다.
     *
     * @param local_lkey 로컬 MR lkey.
     * @param buf        전송할 버퍼.
     * @param len        전송 바이트 수.
     * @param wr_id      작업 ID.
     */
    virtual Task<Result<Completion>> send(
        uint32_t local_lkey, const void* buf,
        size_t   len,        uint64_t wr_id = 0) noexcept = 0;

    /**
     * @brief Recv 요청을 RQ에 포스팅합니다.
     *
     * `send()` 호출 전에 원격 측에서 pre-post해야 합니다.
     *
     * @param local_lkey 수신 버퍼 MR lkey.
     * @param buf        수신 버퍼.
     * @param len        버퍼 크기.
     * @param wr_id      작업 ID.
     */
    virtual Result<void> post_recv(
        uint32_t local_lkey, void* buf,
        size_t   len,        uint64_t wr_id = 0) noexcept = 0;

    // ── CQ Polling ─────────────────────────────────────────────────────────

    /**
     * @brief CQ를 polling하여 완료 이벤트를 최대 `max_wc`개 수집합니다.
     *
     * @param out    완료 이벤트 출력 배열.
     * @param max_wc 최대 수집 수.
     * @returns 수집된 완료 이벤트 수.
     */
    virtual size_t poll_cq(std::span<Completion> out) noexcept = 0;

    // ── 통계 ───────────────────────────────────────────────────────────────

    [[nodiscard]] virtual uint64_t bytes_sent()     const noexcept = 0;
    [[nodiscard]] virtual uint64_t bytes_received() const noexcept = 0;
    [[nodiscard]] virtual uint64_t send_count()     const noexcept = 0;
    [[nodiscard]] virtual uint64_t recv_count()     const noexcept = 0;

protected:
    RDMAContext& ctx_;
};

} // namespace qbuem::rdma

/** @} */
