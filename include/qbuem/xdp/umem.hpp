#pragma once

/**
 * @file qbuem/xdp/umem.hpp
 * @brief AF_XDP UMEM (User Memory) buffer pool.
 * @defgroup qbuem_xdp AF_XDP eXpress Data Path
 * @ingroup qbuem_xdp
 *
 * UMEM is a memory region shared between the kernel and user space.
 * Contiguous memory allocated via `mmap()` is registered with the kernel using
 * `xsk_umem__create()`, allowing packet data to be written directly into this
 * region — no kernel-to-user copy.
 *
 * ### Dependencies
 * - Linux 4.18+ (`AF_XDP` socket support)
 * - Linux 5.4+ (xsk_socket__create_shared, NEED_WAKEUP flag)
 * - libbpf (optional: enabled when `QBUEM_XDP_LIBBPF=ON` CMake option is set)
 *
 * ### CMake Usage
 * ```cmake
 * find_package(qbuem-stack REQUIRED COMPONENTS xdp)
 * target_link_libraries(myapp PRIVATE qbuem-stack::xdp)
 * ```
 *
 * ### Notes
 * - UMEM chunk size must be a power of two (default: 4096 bytes)
 * - 4096-byte alignment is required, similar to `O_DIRECT` file I/O
 * - RLIMIT_MEMLOCK limit must be raised (`ulimit -l unlimited`)
 *
 * @{
 */

#ifdef QBUEM_HAS_XDP

#include <qbuem/common.hpp>
#include <qbuem/core/arena.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <sys/mman.h>

// libbpf AF_XDP header (used only when QBUEM_XDP_LIBBPF=ON)
#ifdef QBUEM_XDP_LIBBPF
#  include <xdp/xsk.h>
#endif

namespace qbuem::xdp {

// ─── Constants ────────────────────────────────────────────────────────────

/** @brief Default UMEM frame size (4 KiB). */
inline constexpr size_t kDefaultFrameSize = 4096;

/** @brief Default Fill Ring / Completion Ring size. */
inline constexpr uint32_t kDefaultRingSize = 2048;

// ─── UmemConfig ───────────────────────────────────────────────────────────

/**
 * @brief UMEM creation configuration.
 */
struct UmemConfig {
    /** @brief Total number of frames. Memory = frame_count × frame_size. */
    uint32_t frame_count  = 4096;

    /** @brief Bytes per frame. Must be a power of 2. */
    uint32_t frame_size   = kDefaultFrameSize;

    /** @brief Fill Ring size (power of 2). */
    uint32_t fill_size    = kDefaultRingSize;

    /** @brief Completion Ring size (power of 2). */
    uint32_t comp_size    = kDefaultRingSize;

    /** @brief Whether to use Huge Pages (reduces TLB misses). */
    bool     use_hugepages = false;
};

// ─── UmemFrame ────────────────────────────────────────────────────────────

/**
 * @brief Reference to a single packet frame within UMEM.
 *
 * Consists of an offset from the UMEM base (`addr`) and the data length (`len`).
 * The actual data pointer is obtained via `Umem::data(frame)`.
 */
struct UmemFrame {
    /** @brief Offset from the UMEM base (bytes). */
    uint64_t addr;

    /** @brief Valid data length (bytes). */
    uint32_t len;

    /** @brief XDP option flags (XDP_OPTIONS_ZEROCOPY, etc.). */
    uint32_t options;
};

// ─── Umem ─────────────────────────────────────────────────────────────────

/**
 * @brief AF_XDP UMEM buffer pool.
 *
 * Allocates contiguous memory via `mmap()` and registers it with the kernel as UMEM.
 * On packet reception the kernel writes data directly into this memory,
 * eliminating all kernel-to-user copies.
 *
 * ### Usage Example
 * @code
 * qbuem::xdp::UmemConfig cfg{
 *     .frame_count  = 4096,
 *     .frame_size   = 4096,
 *     .fill_size    = 2048,
 *     .comp_size    = 2048,
 *     .use_hugepages = true,
 * };
 * auto umem = qbuem::xdp::Umem::create(cfg);
 * if (!umem) { handle_error(); }
 *
 * // Register frames in the Fill Ring (kernel writes packets to these addresses)
 * umem->fill_frames(batch_size);
 * @endcode
 */
class Umem {
public:
    Umem(const Umem&)            = delete;
    Umem& operator=(const Umem&) = delete;

    Umem(Umem&& other) noexcept
        : cfg_(other.cfg_)
        , mem_(other.mem_)
        , mem_size_(other.mem_size_)
#ifdef QBUEM_XDP_LIBBPF
        , umem_(other.umem_)
        , fill_ring_(other.fill_ring_)
        , comp_ring_(other.comp_ring_)
#endif
        , next_free_frame_(other.next_free_frame_)
    {
        other.mem_  = MAP_FAILED;
        other.mem_size_ = 0;
#ifdef QBUEM_XDP_LIBBPF
        other.umem_ = nullptr;
#endif
    }

    ~Umem() { destroy(); }

    // ── Factory ──────────────────────────────────────────────────────────

    /**
     * @brief Creates a UMEM and registers it with the kernel.
     *
     * @param cfg  UMEM configuration.
     * @returns Umem on success, error_code on failure.
     */
    static Result<Umem> create(const UmemConfig& cfg) {
        Umem u(cfg);

        // 1. Allocate aligned contiguous memory via mmap
        // Integer overflow guard: frame_count * frame_size must not wrap.
        if (cfg.frame_count > 0 &&
            static_cast<size_t>(cfg.frame_size) > SIZE_MAX / static_cast<size_t>(cfg.frame_count)) {
            return unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        const size_t total = static_cast<size_t>(cfg.frame_count) * cfg.frame_size;
        int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;
        if (cfg.use_hugepages) {
#ifdef MAP_HUGETLB
            mmap_flags |= MAP_HUGETLB;
#endif
        }
        void* mem = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);
        if (mem == MAP_FAILED) {
            return unexpected(std::error_code(errno, std::system_category()));
        }
        u.mem_      = static_cast<uint8_t*>(mem);
        u.mem_size_ = total;

#ifdef QBUEM_XDP_LIBBPF
        // 2. Register UMEM with the kernel via libbpf
        xsk_umem_config xsk_cfg{};
        xsk_cfg.fill_size      = cfg.fill_size;
        xsk_cfg.comp_size      = cfg.comp_size;
        xsk_cfg.frame_size     = cfg.frame_size;
        xsk_cfg.frame_headroom = 0;
        xsk_cfg.flags          = 0;

        int ret = xsk_umem__create(&u.umem_, mem, total,
                                   &u.fill_ring_, &u.comp_ring_, &xsk_cfg);
        if (ret != 0) {
            ::munmap(mem, total);
            u.mem_ = static_cast<uint8_t*>(MAP_FAILED);
            return unexpected(std::error_code(-ret, std::system_category()));
        }
#endif
        return u;
    }

    // ── Accessors ──────────────────────────────────────────────────────────

    /**
     * @brief Returns the actual data pointer for the given frame offset.
     * @param frame UmemFrame (uses the addr field).
     * @returns Data pointer within UMEM.
     */
    [[nodiscard]] uint8_t* data(const UmemFrame& frame) noexcept {
        return mem_ + frame.addr;
    }

    /** @copydoc data(const UmemFrame&) */
    [[nodiscard]] const uint8_t* data(const UmemFrame& frame) const noexcept {
        return mem_ + frame.addr;
    }

    /** @brief Total allocated memory size (bytes). */
    [[nodiscard]] size_t mem_size() const noexcept { return mem_size_; }

    /** @brief Returns the configuration. */
    [[nodiscard]] const UmemConfig& config() const noexcept { return cfg_; }

    // ── Fill Ring management ─────────────────────────────────────────────

    /**
     * @brief Fills `n` frames into the Fill Ring to notify the kernel of receive readiness.
     *
     * The kernel writes packets to the registered addresses and notifies completion via the Rx Ring.
     *
     * @param n Number of frames to fill. If more than available frames, fills up to the available count.
     * @returns Actual number of frames filled.
     */
    uint32_t fill_frames(uint32_t n) noexcept {
#ifdef QBUEM_XDP_LIBBPF
        uint32_t idx = 0;
        uint32_t reserved = xsk_ring_prod__reserve(&fill_ring_, n, &idx);
        for (uint32_t i = 0; i < reserved; ++i) {
            *xsk_ring_prod__fill_addr(&fill_ring_, idx++) =
                static_cast<uint64_t>(next_free_frame_++) * cfg_.frame_size;
            if (next_free_frame_ >= cfg_.frame_count) {
                next_free_frame_ = 0; // wrap around
            }
        }
        xsk_ring_prod__submit(&fill_ring_, reserved);
        return reserved;
#else
        (void)n;
        return 0;
#endif
    }

    /**
     * @brief Reclaims transmission-completed frames from the Completion Ring.
     *
     * After `send()`, when the kernel completes transmission it records frame addresses in this ring.
     * Reclaimed frames can be reused.
     *
     * @param frames  Array to store the reclaimed frame addresses.
     * @param max_n   Maximum number of frames to reclaim.
     * @returns Actual number of frames reclaimed.
     */
    uint32_t reclaim_tx(uint64_t* frames, uint32_t max_n) noexcept {
#ifdef QBUEM_XDP_LIBBPF
        uint32_t idx = 0;
        uint32_t completed = xsk_ring_cons__peek(&comp_ring_, max_n, &idx);
        for (uint32_t i = 0; i < completed; ++i) {
            frames[i] = *xsk_ring_cons__comp_addr(&comp_ring_, idx++);
        }
        xsk_ring_cons__release(&comp_ring_, completed);
        return completed;
#else
        (void)frames; (void)max_n;
        return 0;
#endif
    }

#ifdef QBUEM_XDP_LIBBPF
    /** @brief Internal xsk_umem* handle (for direct libbpf API usage). */
    [[nodiscard]] xsk_umem* handle() noexcept { return umem_; }

    /** @brief Fill Ring (xsk_ring_prod*) reference. */
    [[nodiscard]] xsk_ring_prod* fill_ring() noexcept { return &fill_ring_; }

    /** @brief Completion Ring (xsk_ring_cons*) reference. */
    [[nodiscard]] xsk_ring_cons* comp_ring() noexcept { return &comp_ring_; }
#endif

private:
    explicit Umem(const UmemConfig& cfg) noexcept
        : cfg_(cfg)
        , mem_(static_cast<uint8_t*>(MAP_FAILED))
        , mem_size_(0)
#ifdef QBUEM_XDP_LIBBPF
        , umem_(nullptr)
        , fill_ring_{}
        , comp_ring_{}
#endif
        , next_free_frame_(0)
    {}

    void destroy() noexcept {
#ifdef QBUEM_XDP_LIBBPF
        if (umem_) {
            xsk_umem__delete(umem_);
            umem_ = nullptr;
        }
#endif
        if (mem_ != static_cast<uint8_t*>(MAP_FAILED) && mem_size_ > 0) {
            ::munmap(mem_, mem_size_);
            mem_      = static_cast<uint8_t*>(MAP_FAILED);
            mem_size_ = 0;
        }
    }

    UmemConfig  cfg_;
    uint8_t*    mem_;
    size_t      mem_size_;

#ifdef QBUEM_XDP_LIBBPF
    xsk_umem*      umem_;
    xsk_ring_prod  fill_ring_;
    xsk_ring_cons  comp_ring_;
#endif

    uint32_t    next_free_frame_;
};

} // namespace qbuem::xdp

#endif // QBUEM_HAS_XDP

/** @} */ // end of qbuem_xdp
