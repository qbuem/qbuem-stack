#pragma once

/**
 * @file qbuem/ebpf/ebpf_tracer.hpp
 * @brief eBPF CO-RE 기반 qbuem-stack 관찰성 — BPF 측정점 및 맵 읽기 인터페이스.
 * @defgroup qbuem_ebpf EBPFTracer
 * @ingroup qbuem_observability
 *
 * ## 개요
 * eBPF CO-RE(Compile Once - Run Everywhere)를 활용해 qbuem-stack의
 * 런타임 이벤트를 kernel-space에서 측정하고 user-space로 전달합니다.
 *
 * ## 아키텍처
 * ```
 * qbuem-stack (user-space)     kernel eBPF programs    user-space consumer
 *  TCP accept ──────────────► kprobe/tracepoint ────► BPF ringbuf ──► EBPFTracer::poll()
 *  HTTP parse  ─────────────► uprobe               ──► BPF hashmap ──► EBPFTracer::read_map()
 *  io_uring SQE ───────────► tracepoint/fentry    ──► perf event  ──► EBPFTracer::subscribe()
 * ```
 *
 * ## CO-RE 호환성
 * BTF(BPF Type Format)를 통해 커널 구조체 오프셋을 런타임에 재배치합니다.
 * 한 번 컴파일된 BPF 오브젝트는 커널 5.4+에서 재컴파일 없이 동작합니다.
 *
 * ## 측정점 카탈로그
 * | 측정점 | 타입 | 이벤트 |
 * |--------|------|--------|
 * | `tcp_accept` | kprobe | 새 TCP 연결 수락 |
 * | `http_parse_latency` | uprobe | HTTP 파싱 레이턴시 |
 * | `pipeline_action_enter/exit` | uprobe | 파이프라인 액션 실행 시간 |
 * | `io_uring_submit` | tracepoint | io_uring SQE 제출 |
 * | `shm_channel_send` | uprobe | SHM 채널 메시지 전송 |
 *
 * @{
 */

#include <qbuem/common.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace qbuem::ebpf {

// ─── 이벤트 타입 ─────────────────────────────────────────────────────────────

/** @brief eBPF 측정점에서 수집된 이벤트 종류. */
enum class EventType : uint16_t {
    TcpAccept          = 1,
    TcpClose           = 2,
    HttpParseBegin     = 3,
    HttpParseEnd       = 4,
    PipelineActionEnter= 5,
    PipelineActionExit = 6,
    IoUringSqeSubmit   = 7,
    IoUringCqeReceive  = 8,
    ShmChannelSend     = 9,
    ShmChannelRecv     = 10,
    RdmaWrite          = 11,
    JwtVerify          = 12,
    Custom             = 255,
};

// ─── 이벤트 레코드 ────────────────────────────────────────────────────────────

/**
 * @brief BPF ringbuf에서 user-space로 전달되는 이벤트 레코드.
 *
 * 크기: 64B (단일 캐시 라인).
 */
struct alignas(64) TraceEvent {
    uint64_t  timestamp_ns{0};  ///< ktime_get_ns() 타임스탬프
    uint64_t  duration_ns{0};   ///< 소요 시간 (enter/exit 쌍인 경우)
    uint64_t  tid{0};           ///< 스레드 ID (BPF bpf_get_current_pid_tgid())
    uint32_t  cpu{0};           ///< 실행된 CPU 번호
    EventType type{EventType::Custom};
    uint16_t  flags{0};         ///< 이벤트별 플래그
    uint8_t   label[24]{};      ///< 이벤트 레이블 (null-terminated, 최대 23자)
    uint64_t  val[2]{};         ///< 이벤트별 추가 데이터 (e.g. fd, bytes)

    /** @brief 레이블을 안전하게 설정합니다. */
    void set_label(std::string_view s) noexcept {
        size_t n = s.size() < 23 ? s.size() : 23;
        __builtin_memcpy(label, s.data(), n);
        label[n] = '\0';
    }
    [[nodiscard]] std::string_view get_label() const noexcept {
        return {reinterpret_cast<const char*>(label)};
    }
};
static_assert(sizeof(TraceEvent) == 64, "TraceEvent must be exactly 64 bytes");

// ─── BPF 맵 통계 ─────────────────────────────────────────────────────────────

/**
 * @brief qbuem BPF 프로그램이 집계하는 전역 통계.
 *
 * BPF hashmap/percpu_array에 저장되며 `EBPFTracer::read_stats()`로 조회합니다.
 */
struct BPFStats {
    uint64_t tcp_accepts{0};         ///< 총 TCP accept 횟수
    uint64_t http_requests{0};       ///< 총 HTTP 요청 수
    uint64_t avg_http_parse_ns{0};   ///< 평균 HTTP 파싱 레이턴시 (ns)
    uint64_t io_uring_submits{0};    ///< io_uring SQE 제출 횟수
    uint64_t pipeline_actions{0};    ///< 파이프라인 액션 실행 횟수
    uint64_t shm_sends{0};          ///< SHM 채널 전송 횟수
    uint64_t jwt_verifications{0};   ///< JWT 검증 횟수
    uint64_t rdma_writes{0};        ///< RDMA Write 횟수
};

// ─── 이벤트 콜백 ─────────────────────────────────────────────────────────────

/** @brief BPF ringbuf 이벤트 수신 콜백. */
using EventCallback = std::function<void(const TraceEvent&)>;

// ─── EBPFTracer ──────────────────────────────────────────────────────────────

/**
 * @brief qbuem-stack eBPF 관찰성 트레이서.
 *
 * ## 라이프사이클
 * 1. `EBPFTracer::create()` — BPF 오브젝트 로드, 맵/프로그램 초기화.
 * 2. `enable()` — 측정점 attach (uprobe/kprobe/tracepoint).
 * 3. `poll()` — BPF ringbuf에서 이벤트 수신.
 * 4. `disable()` — 측정점 detach.
 *
 * ## 권한
 * `CAP_BPF` (Linux 5.8+) 또는 `CAP_SYS_ADMIN`이 필요합니다.
 */
class EBPFTracer {
public:
    /**
     * @brief EBPFTracer를 초기화합니다.
     *
     * @param bpf_obj_path BPF 오브젝트 파일 경로 (`.bpf.o`).
     *                     `""` 이면 내장 skeleton BPF를 사용합니다.
     * @returns 트레이서 또는 에러.
     */
    static Result<std::unique_ptr<EBPFTracer>> create(
        std::string_view bpf_obj_path = "") noexcept;

    virtual ~EBPFTracer() = default;

    // ── 측정점 관리 ────────────────────────────────────────────────────────

    /**
     * @brief 특정 측정점을 활성화합니다.
     *
     * @param event_type 활성화할 이벤트 타입.
     * @returns 성공 시 `Result<void>`.
     */
    virtual Result<void> enable(EventType event_type) noexcept = 0;

    /**
     * @brief 모든 측정점을 활성화합니다.
     */
    virtual Result<void> enable_all() noexcept = 0;

    /**
     * @brief 특정 측정점을 비활성화합니다.
     */
    virtual Result<void> disable(EventType event_type) noexcept = 0;

    /**
     * @brief 모든 측정점을 비활성화합니다.
     */
    virtual void disable_all() noexcept = 0;

    // ── 이벤트 수신 ────────────────────────────────────────────────────────

    /**
     * @brief BPF ringbuf를 polling하여 이벤트를 수신합니다.
     *
     * @param out      이벤트를 저장할 배열.
     * @param timeout_ms polling 타임아웃 (ms). 0이면 논블로킹.
     * @returns 수신된 이벤트 수.
     */
    virtual size_t poll(std::span<TraceEvent> out,
                         int timeout_ms = 0) noexcept = 0;

    /**
     * @brief 이벤트 콜백을 등록합니다.
     *
     * `poll()` 대신 콜백 방식을 선호하는 경우 사용합니다.
     *
     * @param cb 이벤트 수신 시 호출될 콜백.
     */
    virtual void subscribe(EventCallback cb) noexcept = 0;

    // ── 통계 ───────────────────────────────────────────────────────────────

    /**
     * @brief BPF 맵에서 집계 통계를 읽습니다.
     *
     * @returns 현재 BPFStats 스냅샷.
     */
    [[nodiscard]] virtual BPFStats read_stats() const noexcept = 0;

    /**
     * @brief BPF 통계 카운터를 초기화합니다.
     */
    virtual void reset_stats() noexcept = 0;

    // ── BPF 맵 직접 접근 ──────────────────────────────────────────────────

    /**
     * @brief BPF 맵에서 키로 값을 조회합니다.
     *
     * @tparam K 키 타입.
     * @tparam V 값 타입.
     * @param map_name BPF 맵 이름.
     * @param key      조회 키.
     * @returns 값 또는 nullopt (키 없음).
     */
    template <typename K, typename V>
    [[nodiscard]] std::optional<V> lookup_map(std::string_view map_name,
                                               const K& key) const noexcept;

    // ── 진단 ───────────────────────────────────────────────────────────────

    /** @brief 로드된 BPF 프로그램 수. */
    [[nodiscard]] virtual size_t program_count() const noexcept = 0;

    /** @brief 로드된 BPF 맵 수. */
    [[nodiscard]] virtual size_t map_count() const noexcept = 0;

    /** @brief BPF ringbuf 드롭 카운트 (버퍼 포화 시). */
    [[nodiscard]] virtual uint64_t ringbuf_drops() const noexcept = 0;
};

// ─── 경량 측정 매크로 (user-space uprobe 측정점) ──────────────────────────────

/**
 * @brief uprobe 측정점을 정의하는 인라인 no-op 함수.
 *
 * BPF 프로그램은 이 함수에 uprobe를 부착합니다.
 * release 빌드에서도 심볼이 제거되지 않도록 `[[gnu::noinline]]`으로 선언합니다.
 *
 * @code
 * // qbuem 내부에서 사용:
 * QBUEM_TRACE_POINT("pipeline.http.parse.begin", fd, 0);
 * // BPF 프로그램: SEC("uprobe/qbuem_trace_point")
 * @endcode
 */
[[gnu::noinline]] inline void qbuem_trace_point(
    const char* label, uint64_t val0, uint64_t val1) noexcept {
    // 측정점 no-op — BPF uprobe가 부착됩니다.
    // volatile 키워드로 컴파일러 최적화 제거 방지
    volatile const char* l = label;
    volatile uint64_t    v0 = val0;
    volatile uint64_t    v1 = val1;
    (void)l; (void)v0; (void)v1;
}

/**
 * @brief 편의 매크로 — 측정점 호출.
 *
 * 릴리스 빌드에서도 측정점은 유지됩니다 (BPF attach 용).
 * 오버헤드는 단일 no-op 함수 호출(≈1ns)입니다.
 */
#define QBUEM_TRACE(label, val0, val1) \
    ::qbuem::ebpf::qbuem_trace_point((label), (val0), (val1))

} // namespace qbuem::ebpf

/** @} */
