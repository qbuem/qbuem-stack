#pragma once

/**
 * @file qbuem/shm/shm_channel.hpp
 * @brief SHMChannel — 프로세스 간 제로-카피 공유 메모리 채널.
 * @defgroup qbuem_shm_channel SHMChannel
 * @ingroup qbuem_shm
 *
 * ## 개요
 * `SHMChannel<T>`는 `memfd_create(2)` 기반 공유 메모리 세그먼트 위에서
 * 150ns 수준의 p99 IPC 레이턴시를 달성하는 MPMC 채널입니다.
 *
 * ## 세그먼트 레이아웃
 * ```
 * [SHMHeader (64B)] → [MetadataRing (N × 32B)] → [DataArena (가변)]
 * ```
 *
 * ## 동기화
 * - **Producer**: CAS(Tail) → 데이터 쓰기 → Sequence 커밋 → `IORING_OP_FUTEX_WAKE`
 * - **Consumer**: try_recv() → 비면 `IORING_OP_FUTEX_WAIT` co_await → 재개
 *
 * ## 주요 특성
 * - **Zero Allocation**: 핫 패스에서 힙 할당 없음.
 * - **Zero Copy**: Consumer는 DataArena를 직접 뷰로 접근.
 * - **Reactor Integration**: Futex 대기는 io_uring SQE로 발행 → non-blocking.
 *
 * @code
 * // 생산자 프로세스
 * auto ch = SHMChannel<MyMsg>::create("system.events", 1024);
 * co_await ch->send(msg);
 *
 * // 소비자 프로세스
 * auto ch = SHMChannel<MyMsg>::open("system.events");
 * auto view = co_await ch->recv(); // zero-copy: DataArena 직접 뷰
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string_view>

namespace qbuem::shm {

// ─── 상수 ─────────────────────────────────────────────────────────────────────

/** @brief 세그먼트 무결성 검증 매직 넘버 (`QBUM` ASCII). */
inline constexpr uint32_t kSHMMagic = 0x5142554D;

/** @brief 캐시 라인 크기. */
inline constexpr size_t kCacheLineSize = 64;

// ─── SHMHeader ────────────────────────────────────────────────────────────────

/**
 * @brief 공유 메모리 세그먼트 제어 헤더 (offset 0, 64B).
 *
 * 단일 캐시 라인에 핵심 상태를 모두 담아 MESI 프로토콜 충돌을 최소화합니다.
 */
struct alignas(kCacheLineSize) SHMHeader {
    std::atomic<uint64_t> tail{0};      ///< 생산자 커밋 인덱스 (Producers++)
    std::atomic<uint64_t> head{0};      ///< 소비자 소비 인덱스 (Consumers++)
    uint32_t              capacity{0};  ///< 링 버퍼 슬롯 수 (2의 거듭제곱)
    uint32_t              magic{kSHMMagic}; ///< 무결성 검증
    std::atomic<uint32_t> state{1};     ///< bit0: Active, bit1: Draining, bit2: Error
    uint8_t               _pad[64 - sizeof(std::atomic<uint64_t>) * 2
                                   - sizeof(uint32_t) * 2
                                   - sizeof(std::atomic<uint32_t>)]{};
};
static_assert(sizeof(SHMHeader) == kCacheLineSize, "SHMHeader must be exactly 64 bytes");

// ─── MetadataSlot ─────────────────────────────────────────────────────────────

/**
 * @brief 링 버퍼 메타데이터 슬롯 (32B, Vyukov MPMC 시퀀스 방식).
 *
 * 슬롯은 메시지 자체가 아닌 DataArena 위치 정보만 저장합니다.
 */
struct alignas(32) MetadataSlot {
    std::atomic<uint64_t> seq{0};   ///< Vyukov 시퀀스 넘버 (커밋 동기화)
    uint32_t              off{0};   ///< DataArena 내 오프셋 (bytes)
    uint32_t              len{0};   ///< 페이로드 바이트 수
    uint64_t              tid{0};   ///< 타입 ID (스키마 검증)
    uint32_t              flg{0};   ///< 플래그: bit0=Multipart, bit1=Compressed, bit2=Encrypted
    uint32_t              epc{0};   ///< 에폭 (ABA 방지)
};
static_assert(sizeof(MetadataSlot) == 32, "MetadataSlot must be exactly 32 bytes");

// ─── SHMEnvelope ─────────────────────────────────────────────────────────────

/**
 * @brief DataArena 내 메시지 앞에 붙는 128B 컨텍스트 엔벨로프.
 *
 * W3C Trace Context, 스팬 ID, 타입 ID, 인증 토큰을 포함합니다.
 */
struct alignas(16) SHMEnvelope {
    uint8_t  trace_id[16]{};     ///< W3C Trace ID (128-bit)
    uint64_t span_id{0};         ///< Span ID
    uint64_t type_id{0};         ///< 타입 식별자
    uint8_t  auth_token[32]{};   ///< 인증 토큰
    uint8_t  reserved[64]{};     ///< 향후 확장용 패딩 (128B 정렬)
};
static_assert(sizeof(SHMEnvelope) == 128, "SHMEnvelope must be exactly 128 bytes");

// ─── SHMSegment ───────────────────────────────────────────────────────────────

/**
 * @brief 공유 메모리 세그먼트 RAII 관리자.
 *
 * `memfd_create(2)` (Linux) 또는 `shm_open(3)` (macOS)으로 세그먼트를 생성/열기.
 * 소멸 시 `munmap` + `close`가 자동으로 수행됩니다.
 */
class SHMSegment {
public:
    SHMSegment() = default;
    ~SHMSegment();

    SHMSegment(const SHMSegment&) = delete;
    SHMSegment& operator=(const SHMSegment&) = delete;
    SHMSegment(SHMSegment&&) noexcept;
    SHMSegment& operator=(SHMSegment&&) noexcept;

    /**
     * @brief 새 공유 메모리 세그먼트를 생성합니다.
     * @param name  세그먼트 이름 (최대 255자, `/` 포함 가능).
     * @param size  세그먼트 크기 (바이트, 페이지 경계로 올림).
     * @returns 성공 시 세그먼트, 실패 시 에러.
     */
    static Result<SHMSegment> create(std::string_view name, size_t size) noexcept;

    /**
     * @brief 기존 공유 메모리 세그먼트를 엽니다.
     * @param name  세그먼트 이름.
     * @returns 성공 시 세그먼트, 실패 시 에러.
     */
    static Result<SHMSegment> open(std::string_view name) noexcept;

    /** @brief 세그먼트 기저 주소를 반환합니다 (read-write). */
    [[nodiscard]] void* base() const noexcept { return base_; }

    /** @brief 세그먼트 크기를 반환합니다. */
    [[nodiscard]] size_t size() const noexcept { return size_; }

    /** @brief 세그먼트가 유효한지 확인합니다. */
    [[nodiscard]] bool valid() const noexcept { return base_ != nullptr; }

private:
    void*  base_{nullptr};
    size_t size_{0};
    int    fd_{-1};
};

// ─── SHMChannel<T> ────────────────────────────────────────────────────────────

/**
 * @brief 공유 메모리 기반 MPMC 비동기 채널.
 *
 * @tparam T 전송할 타입. `std::is_trivially_copyable<T>`이어야 합니다.
 *
 * ## 핫 패스 (send)
 * 1. `Tail` CAS로 슬롯 확보 (lock-free).
 * 2. `DataArena[off]`에 `T` 직접 memcpy.
 * 3. `slot.seq` 커밋.
 * 4. 소비자가 대기 중이면 `IORING_OP_FUTEX_WAKE`.
 *
 * ## 핫 패스 (recv)
 * 1. `try_recv_view()`: 슬롯 시퀀스 확인 → DataArena 뷰 반환 (복사 없음).
 * 2. 비어 있으면 `IORING_OP_FUTEX_WAIT` co_await.
 */
template <typename T>
class SHMChannel {
    static_assert(std::is_trivially_copyable_v<T>,
                  "SHMChannel<T>: T must be trivially copyable");

public:
    /** @brief 생산자/소비자 측 이름으로 채널을 가리키는 핸들 타입. */
    using Ptr = std::unique_ptr<SHMChannel<T>>;

    /**
     * @brief 새 SHM 채널을 생성합니다 (생산자 측).
     * @param name     채널 이름 (토픽 식별자).
     * @param capacity 링 버퍼 슬롯 수 (2의 거듭제곱으로 올림).
     * @returns 채널 포인터 또는 에러.
     */
    static Result<Ptr> create(std::string_view name, size_t capacity) noexcept;

    /**
     * @brief 기존 SHM 채널을 엽니다 (소비자 측).
     * @param name 채널 이름.
     * @returns 채널 포인터 또는 에러.
     */
    static Result<Ptr> open(std::string_view name) noexcept;

    // ── 생산자 API ──────────────────────────────────────────────────────────

    /**
     * @brief 메시지를 전송합니다.
     *
     * 슬롯이 가득 차면 co_await으로 백프레셔 대기합니다.
     * @param msg 전송할 메시지 (T의 복사본이 DataArena에 저장됨).
     * @returns 성공 또는 에러 (채널 닫힘).
     */
    Task<Result<void>> send(const T& msg) noexcept;

    /**
     * @brief 논블로킹 송신 시도.
     * @returns 성공이면 true, 가득 차거나 닫힌 경우 false.
     */
    bool try_send(const T& msg) noexcept;

    // ── 소비자 API ──────────────────────────────────────────────────────────

    /**
     * @brief 메시지를 수신합니다 (zero-copy 뷰).
     *
     * 채널이 비어 있으면 `IORING_OP_FUTEX_WAIT` co_await으로 대기합니다.
     * 반환된 `const T*`는 DataArena를 직접 가리키며, 다음 recv() 전까지 유효합니다.
     *
     * @returns 메시지 포인터 또는 nullopt (채널 닫힘).
     */
    Task<std::optional<const T*>> recv() noexcept;

    /**
     * @brief 논블로킹 수신 시도 (zero-copy).
     * @returns 데이터 있으면 포인터, 없으면 nullopt.
     */
    std::optional<const T*> try_recv() noexcept;

    // ── 라이프사이클 ────────────────────────────────────────────────────────

    /** @brief 채널을 닫습니다 (Draining 상태로 전환). */
    void close() noexcept;

    /** @brief 채널이 열려 있는지 확인합니다. */
    [[nodiscard]] bool is_open() const noexcept;

    /** @brief 현재 대기 중인 메시지 수를 근사적으로 반환합니다. */
    [[nodiscard]] size_t size_approx() const noexcept;

    /** @brief 링 버퍼 용량을 반환합니다. */
    [[nodiscard]] size_t capacity() const noexcept;

private:
    explicit SHMChannel(SHMSegment seg, size_t capacity) noexcept;

    // 세그먼트 내 주요 영역 포인터 (computed from base)
    SHMHeader*    header() noexcept;
    MetadataSlot* ring() noexcept;
    uint8_t*      arena() noexcept;

    const SHMHeader*    header() const noexcept;
    const MetadataSlot* ring()   const noexcept;
    const uint8_t*      arena()  const noexcept;

    // 슬롯 인덱스 → DataArena 오프셋 계산
    [[nodiscard]] uint32_t slot_to_offset(size_t slot_idx) const noexcept;

    // Futex-uring 동기화 (io_uring 없는 환경에서는 폴백 사용)
    Task<void> futex_wait_recv() noexcept;
    void       futex_wake_recv() noexcept;
    Task<void> futex_wait_send() noexcept;
    void       futex_wake_send() noexcept;

    SHMSegment seg_;
    size_t     capacity_;
};

// ─── 세그먼트 레이아웃 계산 헬퍼 ─────────────────────────────────────────────

/**
 * @brief 주어진 용량과 메시지 크기에 필요한 SHM 세그먼트 총 크기를 계산합니다.
 *
 * @param capacity   링 버퍼 슬롯 수.
 * @param msg_size   메시지(T) 크기 (bytes).
 * @param envelope   `SHMEnvelope`를 앞에 붙일지 여부.
 * @returns 필요한 세그먼트 크기 (페이지 경계로 정렬됨).
 */
[[nodiscard]] size_t calc_segment_size(size_t capacity,
                                        size_t msg_size,
                                        bool   envelope = false) noexcept;

} // namespace qbuem::shm

/** @} */
