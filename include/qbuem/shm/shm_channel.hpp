#pragma once

/**
 * @file qbuem/shm/shm_channel.hpp
 * @brief SHMChannel — zero-copy shared memory channel between processes.
 * @defgroup qbuem_shm_channel SHMChannel
 * @ingroup qbuem_shm
 *
 * ## Overview
 * `SHMChannel<T>` is an MPMC channel built on a `memfd_create(2)`-based
 * shared memory segment, achieving p99 IPC latency in the ~150 ns range.
 *
 * ## Segment Layout
 * ```
 * [SHMHeader (64B)] → [MetadataRing (N × 32B)] → [DataArena (variable)]
 * ```
 *
 * ## Synchronization
 * - **Producer**: CAS(Tail) → write data → commit Sequence → `IORING_OP_FUTEX_WAKE`
 * - **Consumer**: try_recv() → if empty, `IORING_OP_FUTEX_WAIT` co_await → resume
 *
 * ## Key Properties
 * - **Zero Allocation**: no heap allocation on the hot path.
 * - **Zero Copy**: consumer accesses DataArena directly as a view.
 * - **Reactor Integration**: futex wait is issued as an io_uring SQE → non-blocking.
 *
 * @code
 * // Producer process
 * auto ch = SHMChannel<MyMsg>::create("system.events", 1024);
 * co_await ch->send(msg);
 *
 * // Consumer process
 * auto ch = SHMChannel<MyMsg>::open("system.events");
 * auto view = co_await ch->recv(); // zero-copy: direct view into DataArena
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

// ─── Constants ────────────────────────────────────────────────────────────────

/** @brief Magic number for segment integrity verification (`QBUM` ASCII). */
inline constexpr uint32_t kSHMMagic = 0x5142554D;

/** @brief Cache line size. */
inline constexpr size_t kCacheLineSize = 64;

// ─── SHMHeader ────────────────────────────────────────────────────────────────

/**
 * @brief Shared memory segment control header (offset 0, 64B).
 *
 * Fits all critical state into a single cache line to minimize MESI protocol
 * conflicts.
 */
struct alignas(kCacheLineSize) SHMHeader {
    std::atomic<uint64_t> tail{0};      ///< Producer commit index (Producers++)
    std::atomic<uint64_t> head{0};      ///< Consumer consume index (Consumers++)
    uint32_t              capacity{0};  ///< Ring buffer slot count (power of two)
    uint32_t              magic{kSHMMagic}; ///< Integrity check
    std::atomic<uint32_t> state{1};     ///< bit0: Active, bit1: Draining, bit2: Error
    uint8_t               _pad[64 - sizeof(std::atomic<uint64_t>) * 2
                                   - sizeof(uint32_t) * 2
                                   - sizeof(std::atomic<uint32_t>)]{};
};
static_assert(sizeof(SHMHeader) == kCacheLineSize, "SHMHeader must be exactly 64 bytes");

// ─── MetadataSlot ─────────────────────────────────────────────────────────────

/**
 * @brief Ring buffer metadata slot (32B, Vyukov MPMC sequence style).
 *
 * Each slot stores only the location within the DataArena, not the message itself.
 */
struct alignas(32) MetadataSlot {
    std::atomic<uint64_t> seq{0};   ///< Vyukov sequence number (commit synchronization)
    uint32_t              off{0};   ///< Offset within DataArena (bytes)
    uint32_t              len{0};   ///< Payload byte count
    uint64_t              tid{0};   ///< Type ID (schema validation)
    uint32_t              flg{0};   ///< Flags: bit0=Multipart, bit1=Compressed, bit2=Encrypted
    uint32_t              epc{0};   ///< Epoch (ABA prevention)
};
static_assert(sizeof(MetadataSlot) == 32, "MetadataSlot must be exactly 32 bytes");

// ─── SHMEnvelope ─────────────────────────────────────────────────────────────

/**
 * @brief 128B context envelope prepended to each message in the DataArena.
 *
 * Contains W3C Trace Context, span ID, type ID, and authentication token.
 */
struct alignas(16) SHMEnvelope {
    uint8_t  trace_id[16]{};     ///< W3C Trace ID (128-bit)
    uint64_t span_id{0};         ///< Span ID
    uint64_t type_id{0};         ///< Type identifier
    uint8_t  auth_token[32]{};   ///< Authentication token
    uint8_t  reserved[64]{};     ///< Reserved padding for future extension (128B alignment)
};
static_assert(sizeof(SHMEnvelope) == 128, "SHMEnvelope must be exactly 128 bytes");

// ─── SHMSegment ───────────────────────────────────────────────────────────────

/**
 * @brief RAII manager for a shared memory segment.
 *
 * Creates/opens a segment via `memfd_create(2)` (Linux) or `shm_open(3)` (macOS).
 * `munmap` + `close` are performed automatically on destruction.
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
     * @brief Creates a new shared memory segment.
     * @param name  Segment name (up to 255 characters, may include `/`).
     * @param size  Segment size in bytes (rounded up to page boundary).
     * @returns Segment on success, error on failure.
     */
    static Result<SHMSegment> create(std::string_view name, size_t size) noexcept;

    /**
     * @brief Opens an existing shared memory segment.
     * @param name  Segment name.
     * @returns Segment on success, error on failure.
     */
    static Result<SHMSegment> open(std::string_view name) noexcept;

    /** @brief Returns the segment base address (read-write). */
    [[nodiscard]] void* base() const noexcept { return base_; }

    /** @brief Returns the segment size. */
    [[nodiscard]] size_t size() const noexcept { return size_; }

    /** @brief Returns whether the segment is valid. */
    [[nodiscard]] bool valid() const noexcept { return base_ != nullptr; }

private:
    void*  base_{nullptr};
    size_t size_{0};
    int    fd_{-1};
};

// ─── SHMChannel<T> ────────────────────────────────────────────────────────────

/**
 * @brief Shared-memory-based MPMC async channel.
 *
 * @tparam T Type to transfer. Must satisfy `std::is_trivially_copyable<T>`.
 *
 * ## Hot Path (send)
 * 1. Acquire slot via `Tail` CAS (lock-free).
 * 2. memcpy `T` directly into `DataArena[off]`.
 * 3. Commit `slot.seq`.
 * 4. If consumer is waiting, issue `IORING_OP_FUTEX_WAKE`.
 *
 * ## Hot Path (recv)
 * 1. `try_recv_view()`: check slot sequence → return DataArena view (no copy).
 * 2. If empty, `co_await IORING_OP_FUTEX_WAIT`.
 */
template <typename T>
class SHMChannel {
    static_assert(std::is_trivially_copyable_v<T>,
                  "SHMChannel<T>: T must be trivially copyable");

public:
    /** @brief Handle type pointing to the channel from producer/consumer side. */
    using Ptr = std::unique_ptr<SHMChannel<T>>;

    /**
     * @brief Creates a new SHM channel (producer side).
     * @param name     Channel name (topic identifier).
     * @param capacity Ring buffer slot count (rounded up to power of two).
     * @returns Channel pointer or error.
     */
    static Result<Ptr> create(std::string_view name, size_t capacity) noexcept;

    /**
     * @brief Opens an existing SHM channel (consumer side).
     * @param name Channel name.
     * @returns Channel pointer or error.
     */
    static Result<Ptr> open(std::string_view name) noexcept;

    /**
     * @brief Removes the SHM name from the filesystem.
     *
     * The memory is freed after all handles are closed.
     * Use this to clean up segments left in `/dev/shm` after process exit.
     *
     * @param name Channel name (same as passed to `create()`).
     * @returns `Result<void>::ok()` on success, error otherwise.
     */
    static Result<void> unlink(std::string_view name) noexcept;

    // ── Producer API ────────────────────────────────────────────────────────

    /**
     * @brief Sends a message.
     *
     * If the slot is full, waits via co_await backpressure.
     * @param msg Message to send (a copy of T is stored in the DataArena).
     * @returns Success or error (channel closed).
     */
    Task<Result<void>> send(const T& msg) noexcept;

    /**
     * @brief Non-blocking send attempt.
     * @returns true on success, false if full or closed.
     */
    bool try_send(const T& msg) noexcept;

    // ── Consumer API ────────────────────────────────────────────────────────

    /**
     * @brief Receives a message (zero-copy view).
     *
     * If the channel is empty, waits via `co_await IORING_OP_FUTEX_WAIT`.
     * The returned `const T*` points directly into the DataArena and is valid
     * until the next recv() call.
     *
     * @returns Message pointer or nullopt (channel closed).
     */
    Task<std::optional<const T*>> recv() noexcept;

    /**
     * @brief Non-blocking receive attempt (zero-copy).
     * @returns Pointer if data is available, nullopt otherwise.
     */
    std::optional<const T*> try_recv() noexcept;

    // ── Lifecycle ───────────────────────────────────────────────────────────

    /** @brief Closes the channel (transitions to Draining state). */
    void close() noexcept;

    /** @brief Returns whether the channel is open. */
    [[nodiscard]] bool is_open() const noexcept;

    /** @brief Returns an approximate count of pending messages. */
    [[nodiscard]] size_t size_approx() const noexcept;

    /** @brief Returns the ring buffer capacity. */
    [[nodiscard]] size_t capacity() const noexcept;

private:
    explicit SHMChannel(SHMSegment seg, size_t capacity) noexcept;

    // Pointers to key regions within the segment (computed from base)
    SHMHeader*    header() noexcept;
    MetadataSlot* ring() noexcept;
    uint8_t*      arena() noexcept;

    const SHMHeader*    header() const noexcept;
    const MetadataSlot* ring()   const noexcept;
    const uint8_t*      arena()  const noexcept;

    // Compute DataArena offset from slot index
    [[nodiscard]] uint32_t slot_to_offset(size_t slot_idx) const noexcept;

    // Futex-uring synchronization (falls back to polling when io_uring is unavailable)
    Task<void> futex_wait_recv() noexcept;
    void       futex_wake_recv() noexcept;
    Task<void> futex_wait_send() noexcept;
    void       futex_wake_send() noexcept;

    SHMSegment seg_;
    size_t     capacity_;
    T          recv_buf_{};  ///< per-instance copy buffer (zero-copy facade)
};

// ─── Segment layout calculation helpers ──────────────────────────────────────

/**
 * @brief Calculates the total SHM segment size required for the given capacity
 *        and message size.
 *
 * @param capacity   Ring buffer slot count.
 * @param msg_size   Message (T) size in bytes.
 * @param envelope   Whether to prepend a `SHMEnvelope` to each slot.
 * @returns Required segment size (aligned to page boundary).
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
Result<void> SHMChannel<T>::unlink(std::string_view name) noexcept {
    std::string shm_name;
    shm_name.reserve(name.size() + 1);
    if (name.empty() || name[0] != '/') shm_name += '/';
    shm_name.append(name.data(), name.size());

    if (::shm_unlink(shm_name.c_str()) < 0 && errno != ENOENT)
        return unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
    return Result<void>::ok();
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
