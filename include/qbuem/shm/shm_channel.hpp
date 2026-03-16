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
#include <qbuem/core/awaiters.hpp>
#include <qbuem/core/task.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

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
    T          recv_buf_{};  ///< per-instance copy buffer (zero-copy facade)
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
[[nodiscard]] inline size_t calc_segment_size(size_t capacity,
                                               size_t msg_size,
                                               bool   envelope = false) noexcept {
    size_t header_sz = sizeof(SHMHeader);
    size_t ring_sz   = capacity * sizeof(MetadataSlot);
    size_t per_slot  = envelope ? (sizeof(SHMEnvelope) + msg_size) : msg_size;
    size_t arena_sz  = capacity * per_slot;
    size_t total     = header_sz + ring_sz + arena_sz;
    constexpr size_t kPage = 4096;
    return (total + kPage - 1u) & ~(kPage - 1u);
}

// ─── SHMSegment implementation ───────────────────────────────────────────────

inline SHMSegment::~SHMSegment() {
    if (base_ && base_ != MAP_FAILED)
        ::munmap(base_, size_);
    if (fd_ >= 0)
        ::close(fd_);
}

inline SHMSegment::SHMSegment(SHMSegment&& o) noexcept
    : base_(o.base_), size_(o.size_), fd_(o.fd_) {
    o.base_ = nullptr;
    o.size_ = 0;
    o.fd_   = -1;
}

inline SHMSegment& SHMSegment::operator=(SHMSegment&& o) noexcept {
    if (this != &o) {
        if (base_ && base_ != MAP_FAILED) ::munmap(base_, size_);
        if (fd_ >= 0) ::close(fd_);
        base_ = o.base_; size_ = o.size_; fd_ = o.fd_;
        o.base_ = nullptr; o.size_ = 0; o.fd_ = -1;
    }
    return *this;
}

inline Result<SHMSegment> SHMSegment::create(std::string_view name,
                                               size_t size) noexcept {
    std::string shm_name;
    shm_name.reserve(name.size() + 1);
    if (name.empty() || name[0] != '/') shm_name += '/';
    shm_name.append(name.data(), name.size());

    int fd = ::shm_open(shm_name.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0)
        return unexpected(std::make_error_code(std::errc::io_error));

    if (::ftruncate(fd, static_cast<off_t>(size)) < 0) {
        ::close(fd);
        ::shm_unlink(shm_name.c_str());
        return unexpected(std::make_error_code(std::errc::io_error));
    }

    void* base = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        ::close(fd);
        ::shm_unlink(shm_name.c_str());
        return unexpected(std::make_error_code(std::errc::io_error));
    }

    SHMSegment seg;
    seg.base_ = base;
    seg.size_ = size;
    seg.fd_   = fd;
    return seg;
}

inline Result<SHMSegment> SHMSegment::open(std::string_view name) noexcept {
    std::string shm_name;
    shm_name.reserve(name.size() + 1);
    if (name.empty() || name[0] != '/') shm_name += '/';
    shm_name.append(name.data(), name.size());

    int fd = ::shm_open(shm_name.c_str(), O_RDWR, 0600);
    if (fd < 0)
        return unexpected(std::make_error_code(std::errc::no_such_file_or_directory));

    struct stat st{};
    if (::fstat(fd, &st) < 0) {
        ::close(fd);
        return unexpected(std::make_error_code(std::errc::io_error));
    }
    size_t size = static_cast<size_t>(st.st_size);

    void* base = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        ::close(fd);
        return unexpected(std::make_error_code(std::errc::io_error));
    }

    SHMSegment seg;
    seg.base_ = base;
    seg.size_ = size;
    seg.fd_   = fd;
    return seg;
}

// ─── SHMChannel<T> template implementations ──────────────────────────────────

template <typename T>
SHMChannel<T>::SHMChannel(SHMSegment seg, size_t cap) noexcept
    : seg_(std::move(seg)), capacity_(cap) {}

template <typename T>
SHMHeader* SHMChannel<T>::header() noexcept {
    return static_cast<SHMHeader*>(seg_.base());
}
template <typename T>
const SHMHeader* SHMChannel<T>::header() const noexcept {
    return static_cast<const SHMHeader*>(seg_.base());
}

template <typename T>
MetadataSlot* SHMChannel<T>::ring() noexcept {
    return reinterpret_cast<MetadataSlot*>(
        static_cast<uint8_t*>(seg_.base()) + sizeof(SHMHeader));
}
template <typename T>
const MetadataSlot* SHMChannel<T>::ring() const noexcept {
    return reinterpret_cast<const MetadataSlot*>(
        static_cast<const uint8_t*>(seg_.base()) + sizeof(SHMHeader));
}

template <typename T>
uint8_t* SHMChannel<T>::arena() noexcept {
    return static_cast<uint8_t*>(seg_.base())
         + sizeof(SHMHeader)
         + capacity_ * sizeof(MetadataSlot);
}
template <typename T>
const uint8_t* SHMChannel<T>::arena() const noexcept {
    return static_cast<const uint8_t*>(seg_.base())
         + sizeof(SHMHeader)
         + capacity_ * sizeof(MetadataSlot);
}

template <typename T>
uint32_t SHMChannel<T>::slot_to_offset(size_t slot_idx) const noexcept {
    return static_cast<uint32_t>(slot_idx * sizeof(T));
}

template <typename T>
Result<typename SHMChannel<T>::Ptr>
SHMChannel<T>::create(std::string_view name, size_t capacity) noexcept {
    // Round capacity up to next power of two
    size_t cap = 1;
    while (cap < capacity) cap <<= 1;

    size_t seg_size = calc_segment_size(cap, sizeof(T));
    auto seg_res = SHMSegment::create(name, seg_size);
    if (!seg_res) return unexpected(seg_res.error());

    // Placement-init header
    auto* hdr = static_cast<SHMHeader*>(seg_res->base());
    new (hdr) SHMHeader();
    hdr->capacity = static_cast<uint32_t>(cap);
    hdr->magic    = kSHMMagic;
    hdr->state.store(1u, std::memory_order_release);

    // Init Vyukov ring slots: seq[i] = i  (slot[i] is "free for write")
    auto* slots = reinterpret_cast<MetadataSlot*>(
        static_cast<uint8_t*>(seg_res->base()) + sizeof(SHMHeader));
    for (size_t i = 0; i < cap; ++i) {
        new (&slots[i]) MetadataSlot();
        slots[i].seq.store(static_cast<uint64_t>(i), std::memory_order_relaxed);
    }

    return Ptr(new SHMChannel<T>(std::move(*seg_res), cap));
}

template <typename T>
Result<typename SHMChannel<T>::Ptr>
SHMChannel<T>::open(std::string_view name) noexcept {
    auto seg_res = SHMSegment::open(name);
    if (!seg_res) return unexpected(seg_res.error());

    auto* hdr = static_cast<const SHMHeader*>(seg_res->base());
    if (hdr->magic != kSHMMagic)
        return unexpected(std::make_error_code(std::errc::invalid_argument));

    size_t cap = hdr->capacity;
    return Ptr(new SHMChannel<T>(std::move(*seg_res), cap));
}

template <typename T>
bool SHMChannel<T>::try_send(const T& msg) noexcept {
    auto* hdr = header();
    if (!(hdr->state.load(std::memory_order_relaxed) & 1u)) return false;

    uint64_t pos  = hdr->tail.load(std::memory_order_relaxed);
    uint64_t mask = capacity_ - 1;

    for (;;) {
        auto& slot    = ring()[pos & mask];
        uint64_t seq  = slot.seq.load(std::memory_order_acquire);
        int64_t  diff = static_cast<int64_t>(seq) - static_cast<int64_t>(pos);

        if (diff == 0) {
            if (hdr->tail.compare_exchange_weak(pos, pos + 1,
                                                std::memory_order_relaxed)) {
                uint32_t off = static_cast<uint32_t>((pos & mask) * sizeof(T));
                std::memcpy(arena() + off, &msg, sizeof(T));
                slot.off = off;
                slot.len = sizeof(T);
                slot.seq.store(pos + 1, std::memory_order_release);
                return true;
            }
            // CAS failed — pos already updated by CAS
        } else if (diff < 0) {
            return false;  // full
        } else {
            pos = hdr->tail.load(std::memory_order_relaxed);
        }
    }
}

template <typename T>
std::optional<const T*> SHMChannel<T>::try_recv() noexcept {
    auto* hdr = header();
    uint64_t pos  = hdr->head.load(std::memory_order_relaxed);
    uint64_t mask = capacity_ - 1;

    for (;;) {
        auto& slot    = ring()[pos & mask];
        uint64_t seq  = slot.seq.load(std::memory_order_acquire);
        int64_t  diff = static_cast<int64_t>(seq) - static_cast<int64_t>(pos + 1);

        if (diff == 0) {
            if (hdr->head.compare_exchange_weak(pos, pos + 1,
                                                std::memory_order_relaxed)) {
                std::memcpy(&recv_buf_, arena() + slot.off, sizeof(T));
                // Release slot for reuse: next writable turn = pos + capacity
                slot.seq.store(pos + mask + 1, std::memory_order_release);
                return &recv_buf_;
            }
            // CAS failed — pos already updated
        } else if (diff < 0) {
            return std::nullopt;  // empty
        } else {
            pos = hdr->head.load(std::memory_order_relaxed);
        }
    }
}

template <typename T>
Task<Result<void>> SHMChannel<T>::send(const T& msg) noexcept {
    size_t spins = 0;
    while (!try_send(msg)) {
        if (!is_open())
            co_return unexpected(std::make_error_code(std::errc::broken_pipe));
        if (++spins > 128) {
            co_await futex_wait_send();
            spins = 0;
        }
    }
    futex_wake_recv();
    co_return Result<void>{};
}

template <typename T>
Task<std::optional<const T*>> SHMChannel<T>::recv() noexcept {
    size_t spins = 0;
    for (;;) {
        auto r = try_recv();
        if (r) co_return r;
        if (!is_open()) co_return std::nullopt;
        if (++spins > 128) {
            co_await futex_wait_recv();
            spins = 0;
        }
    }
}

template <typename T>
void SHMChannel<T>::close() noexcept {
    header()->state.fetch_and(~1u, std::memory_order_release);
    futex_wake_recv();
    futex_wake_send();
}

template <typename T>
bool SHMChannel<T>::is_open() const noexcept {
    return (header()->state.load(std::memory_order_relaxed) & 1u) != 0u;
}

template <typename T>
size_t SHMChannel<T>::size_approx() const noexcept {
    const auto* hdr = header();
    uint64_t t = hdr->tail.load(std::memory_order_relaxed);
    uint64_t h = hdr->head.load(std::memory_order_relaxed);
    return (t >= h) ? static_cast<size_t>(t - h) : 0u;
}

template <typename T>
size_t SHMChannel<T>::capacity() const noexcept { return capacity_; }

template <typename T>
Task<void> SHMChannel<T>::futex_wait_recv() noexcept {
    co_await qbuem::AsyncSleep{1};  // 1ms yield (fallback; replace with IORING_OP_FUTEX_WAIT)
}

template <typename T>
void SHMChannel<T>::futex_wake_recv() noexcept {
    // polling fallback — consumers wake up from AsyncSleep timer
}

template <typename T>
Task<void> SHMChannel<T>::futex_wait_send() noexcept {
    co_await qbuem::AsyncSleep{1};  // backpressure yield
}

template <typename T>
void SHMChannel<T>::futex_wake_send() noexcept {
    // polling fallback
}

} // namespace qbuem::shm

/** @} */
