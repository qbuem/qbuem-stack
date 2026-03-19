#pragma once

/**
 * @file qbuem/shm/reliable_cast.hpp
 * @brief v2.6.0 ReliableCast<T> — zero-copy 1:N SHM multicast
 * @defgroup qbuem_reliable_cast ReliableCast
 * @ingroup qbuem_shm
 *
 * ## Overview
 *
 * `ReliableCast<T>` implements a **single-writer, N-reader** ring buffer in
 * shared memory where:
 *
 * - The producer writes each item **once** to a shared ring buffer.
 * - Each consumer has its own `head` cursor (stored in the shared segment)
 *   and reads directly from the producer's pages — **zero copy, zero
 *   duplication**.
 * - Reliability policy is configurable per consumer:
 *   - `DropSlow` — producer overwrites; slow consumers lose messages.
 *   - `BlockSlow` — producer blocks if any consumer is still reading.
 *   - `BestEffort` — producer moves on after a brief spin (configurable).
 *
 * ## Memory layout (shared segment)
 * ```
 * [Header (64B aligned)]
 *   magic, capacity, item_size, producer_tail (atomic)
 * [Consumer heads: N × alignas(64) atomic<uint64_t>]
 * [Ring slots: capacity × item_size (page-aligned)]
 * ```
 *
 * ## Constraints
 * - `T` must satisfy `std::is_trivially_copyable_v<T>`.
 * - Maximum consumers: `kMaxConsumers = 64`.
 * - Maximum capacity: power-of-two (enforced at construction).
 *
 * ## Usage Example
 * @code
 * // Producer process
 * ReliableCast<MarketData> cast("mktdata", 4096, ReliableCast<MarketData>::Create);
 * cast.publish(tick);
 *
 * // Consumer process (2 independent consumers)
 * ReliableCast<MarketData> sub0("mktdata", 0, ReliableCast<MarketData>::Attach, 0);
 * ReliableCast<MarketData> sub1("mktdata", 0, ReliableCast<MarketData>::Attach, 1);
 *
 * while (true) {
 *     if (auto data = sub0.try_consume(); data)
 *         process(*data);
 * }
 * @endcode
 *
 * @{
 */

#include <qbuem/common.hpp>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

// POSIX SHM
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace qbuem {

// ─── SlowPolicy ──────────────────────────────────────────────────────────────

/**
 * @brief Determines what happens when the producer is faster than a consumer.
 */
enum class SlowPolicy : uint8_t {
  DropSlow,    ///< Overwrite unconditionally; slow consumers lose messages
  BlockSlow,   ///< Producer spins until the slowest consumer catches up
  BestEffort,  ///< Producer spins up to `spin_limit` times then overwrites
};

// ─── ReliableCast<T> ──────────────────────────────────────────────────────────

/**
 * @brief Zero-copy 1:N SHM multicast ring buffer.
 *
 * @tparam T  Item type. Must be `std::is_trivially_copyable_v<T>`.
 */
template <typename T>
class ReliableCast {
  static_assert(std::is_trivially_copyable_v<T>,
                "ReliableCast<T>: T must be trivially copyable (no hidden copies in SHM)");

public:
  static constexpr size_t kMaxConsumers = 64;

  /** @brief Open mode for the constructor. */
  enum Mode : uint8_t { Create, Attach };

  /**
   * @brief Configuration.
   */
  struct Config {
    SlowPolicy policy      = SlowPolicy::DropSlow; ///< Slow-consumer policy
    uint32_t   spin_limit  = 1024;                 ///< Max spins before overwrite (BestEffort)
    uint32_t   num_consumers = 1;                  ///< Number of consumers to track (Create mode)
  };

  // ── Construction / destruction ────────────────────────────────────────────

  /**
   * @brief Create or attach to a named SHM multicast channel.
   *
   * @param name         SHM name (without leading slash; one is prepended).
   * @param capacity     Ring buffer capacity (power-of-two, Create mode only).
   * @param mode         `Create` initialises; `Attach` maps existing segment.
   * @param consumer_id  Consumer slot index (Attach mode; ignored for Create).
   * @param cfg          Configuration (Create mode only).
   */
  ReliableCast(std::string_view name,
               size_t capacity,
               Mode mode,
               size_t consumer_id = 0,
               Config cfg = {})
      : name_("/" + std::string(name))
      , consumer_id_(consumer_id)
      , cfg_(cfg)
  {
    if (mode == Create) {
      capacity_ = next_pow2(capacity);
      create_segment();
    } else {
      attach_segment();
    }
  }

  ~ReliableCast() {
    if (map_ && map_ != MAP_FAILED) {
      munmap(map_, mapped_size_);
      map_ = nullptr;
    }
  }

  // Non-copyable
  ReliableCast(const ReliableCast&) = delete;
  ReliableCast& operator=(const ReliableCast&) = delete;

  /**
   * @brief Remove the underlying SHM object (call from producer on shutdown).
   */
  void unlink() noexcept { shm_unlink(name_.c_str()); }

  // ── Producer API ──────────────────────────────────────────────────────────

  /**
   * @brief Publish one item to all consumers.
   *
   * The item is written once into the next slot; consumers read from the same
   * physical pages.  Thread-safe for a single producer.
   *
   * @param item  Item to publish.
   */
  void publish(const T& item) noexcept {
    assert(header_ && "ReliableCast not initialised");

    const uint64_t tail  = header_->producer_tail.load(std::memory_order_relaxed);
    const uint64_t slot  = tail & (capacity_ - 1);

    // Respect slow-consumer policy before overwriting
    if (cfg_.policy != SlowPolicy::DropSlow) {
      const uint32_t max_spins =
          (cfg_.policy == SlowPolicy::BestEffort) ? cfg_.spin_limit : UINT32_MAX;
      const uint32_t n = header_->num_consumers;
      for (uint32_t c = 0; c < n; ++c) {
        uint32_t spins = 0;
        while (tail - consumer_heads_[c].load(std::memory_order_acquire) >= capacity_) {
          if (spins++ >= max_spins) break;
          // Busy-spin: no syscall, this is a real-time path
          __asm__ volatile("pause" ::: "memory");
        }
      }
    }

    T* dst = slots_ + slot;
    std::memcpy(dst, &item, sizeof(T));

    header_->producer_tail.store(tail + 1, std::memory_order_release);
  }

  // ── Consumer API ──────────────────────────────────────────────────────────

  /**
   * @brief Try to consume the next available item for this consumer.
   *
   * @return  `std::optional<T>` with the item, or `std::nullopt` if the ring
   *          is empty (no new items since the last `try_consume` call).
   */
  [[nodiscard]] std::optional<T> try_consume() noexcept {
    assert(header_ && "ReliableCast not initialised");
    assert(consumer_id_ < kMaxConsumers);

    auto& head = consumer_heads_[consumer_id_];
    const uint64_t h = head.load(std::memory_order_relaxed);
    const uint64_t t = header_->producer_tail.load(std::memory_order_acquire);

    if (h == t) return std::nullopt; // Ring empty

    // Check for overflow (producer lapped this consumer)
    if (t - h > capacity_) {
      // Jump ahead: skip to the oldest available slot
      head.store(t - capacity_, std::memory_order_relaxed);
      return std::nullopt; // Indicate gap; caller may log a drop
    }

    const uint64_t slot = h & (capacity_ - 1);
    T item;
    std::memcpy(&item, slots_ + slot, sizeof(T));
    head.store(h + 1, std::memory_order_release);
    return item;
  }

  /**
   * @brief Return the number of unread items available for this consumer.
   */
  [[nodiscard]] uint64_t available() const noexcept {
    if (!header_) return 0;
    const uint64_t h = consumer_heads_[consumer_id_].load(std::memory_order_relaxed);
    const uint64_t t = header_->producer_tail.load(std::memory_order_acquire);
    return (t > h) ? t - h : 0;
  }

  /** @brief Return ring buffer capacity (number of slots). */
  [[nodiscard]] size_t capacity() const noexcept { return capacity_; }

private:
  // ── Shared memory layout ──────────────────────────────────────────────────

  static constexpr uint64_t kMagic = 0x5152'4354'4153'5400ULL; // "QRCTAST\0"

  struct alignas(64) Header {
    uint64_t                  magic{kMagic};
    uint64_t                  capacity{0};
    uint64_t                  item_size{sizeof(T)};
    uint32_t                  num_consumers{1};
    uint32_t                  _pad{0};
    alignas(64) std::atomic<uint64_t> producer_tail{0};
  };

  // Per-consumer head cursors are placed immediately after the Header,
  // each on its own cache line to prevent false sharing.
  struct alignas(64) ConsumerHead {
    std::atomic<uint64_t> value{0};
    // Pad to full cache line (std::atomic<uint64_t> = 8 bytes; pad 56 bytes)
    char _pad[56];
  };

  // ── Members ───────────────────────────────────────────────────────────────

  std::string    name_;
  size_t         consumer_id_{0};
  Config         cfg_;
  size_t         capacity_{0};
  size_t         mapped_size_{0};
  void*          map_{nullptr};
  Header*        header_{nullptr};
  ConsumerHead*  consumer_heads_{nullptr};
  T*             slots_{nullptr};

  // ── Helpers ───────────────────────────────────────────────────────────────

  [[nodiscard]] static size_t next_pow2(size_t n) noexcept {
    if (n == 0) return 1;
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
  }

  void compute_layout() noexcept {
    // Header (64B) + N×ConsumerHead (each 64B) + capacity×sizeof(T) (page-aligned)
    const size_t heads_size = kMaxConsumers * sizeof(ConsumerHead);
    const size_t slots_size = capacity_ * sizeof(T);
    mapped_size_ = sizeof(Header) + heads_size + slots_size;
    // Round up to page boundary
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    mapped_size_ = (mapped_size_ + page - 1) & ~(page - 1);
  }

  void map_pointers() noexcept {
    auto* base = static_cast<char*>(map_);
    header_         = reinterpret_cast<Header*>(base);
    consumer_heads_ = reinterpret_cast<ConsumerHead*>(base + sizeof(Header));
    slots_          = reinterpret_cast<T*>(
                        base + sizeof(Header) + kMaxConsumers * sizeof(ConsumerHead));
  }

  void create_segment() {
    compute_layout();
    const int fd = ::shm_open(name_.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0)
      throw std::system_error(errno, std::system_category(), "shm_open(create)");

    if (::ftruncate(fd, static_cast<off_t>(mapped_size_)) != 0) {
      ::close(fd);
      throw std::system_error(errno, std::system_category(), "ftruncate");
    }

    map_ = ::mmap(nullptr, mapped_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (map_ == MAP_FAILED)
      throw std::system_error(errno, std::system_category(), "mmap");

    map_pointers();

    // Initialise header (placement new for atomic members)
    new (header_) Header{};
    header_->capacity      = capacity_;
    header_->num_consumers = static_cast<uint32_t>(cfg_.num_consumers);

    // Initialise consumer heads
    for (size_t i = 0; i < kMaxConsumers; ++i)
      new (&consumer_heads_[i]) ConsumerHead{};
  }

  void attach_segment() {
    // Open existing segment and read its header to discover capacity
    const int fd = ::shm_open(name_.c_str(), O_RDWR, 0600);
    if (fd < 0)
      throw std::system_error(errno, std::system_category(), "shm_open(attach)");

    // Map just the header first to read capacity
    void* hdr_map = ::mmap(nullptr, sizeof(Header), PROT_READ, MAP_SHARED, fd, 0);
    if (hdr_map == MAP_FAILED) { ::close(fd); throw std::system_error(errno, std::system_category(), "mmap(hdr)"); }
    capacity_ = reinterpret_cast<const Header*>(hdr_map)->capacity;
    ::munmap(hdr_map, sizeof(Header));

    compute_layout();

    map_ = ::mmap(nullptr, mapped_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (map_ == MAP_FAILED)
      throw std::system_error(errno, std::system_category(), "mmap");

    map_pointers();
  }
};

/** @} */

} // namespace qbuem
