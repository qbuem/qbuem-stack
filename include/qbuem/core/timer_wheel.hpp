#pragma once

/**
 * @file qbuem/core/timer_wheel.hpp
 * @brief 4-level hierarchical timing wheel — O(1) schedule/cancel
 * @defgroup qbuem_timer TimerWheel
 * @ingroup qbuem_core
 *
 * A 4-level × 256-slot hierarchical timing wheel implementation.
 *
 * ## Slot Resolution
 * - Level 0: 1 ms granularity  (max 256 ms)
 * - Level 1: 256 ms granularity (max 65.536 s)
 * - Level 2: 65.536 s granularity (max ~4.7 hours)
 * - Level 3: ~4.7 hour granularity (max ~49.7 days)
 *
 * ## Complexity
 * - schedule(): O(1)
 * - cancel():   O(entries in slot) worst case, practically O(1)
 * - tick():     O(fired_count)
 *
 * ## Thread Safety
 * TimerWheel is **single-threaded only**. `schedule()`, `cancel()`, `tick()`,
 * and `next_expiry_ms()` must all be called from the same thread (the Reactor
 * event loop). Concurrent access from multiple threads causes TSan data races.
 *
 * ## FixedPoolResource Integration
 * Internal `Entry` objects are allocated from `FixedPoolResource` defined in
 * `<qbuem/core/arena.hpp>`, achieving O(1) allocation/deallocation without
 * heap fragmentation. The pool size is specified via the `pool_capacity`
 * constructor argument.
 *
 * ## Reactor Integration
 * Used as a high-performance timer backend replacing `Reactor::register_timer()`.
 * Reactor implementations call `tick(elapsed_ms)` in the `poll()` loop and
 * pass `next_expiry_ms()` as the poll timeout.
 *
 * @code
 * // Integration pattern in the Reactor event loop:
 * TimerWheel wheel;
 * while (running) {
 *     uint64_t timeout = wheel.next_expiry_ms();
 *     int n = epoll_wait(epfd, events, MAX_EVENTS, (int)std::min(timeout, (uint64_t)INT_MAX));
 *     uint64_t now = monotonic_ms();
 *     wheel.tick(now - last_tick_ms);
 *     last_tick_ms = now;
 *     // process fd events ...
 * }
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/arena.hpp>

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <unordered_map>

namespace qbuem {

/**
 * @brief 4-level hierarchical timing wheel.
 *
 * A timer structure supporting O(1) schedule/cancel without heap fragmentation.
 * Internally uses `FixedPoolResource` to allocate Entry objects from a pool.
 * Integrated with the Reactor event loop for timeout handling.
 *
 * ### Usage Example
 * @code
 * TimerWheel wheel;
 * auto id = wheel.schedule(100, []{ std::puts("100 ms elapsed"); });
 * wheel.tick(50);   // 50 ms elapsed
 * wheel.tick(50);   // 100 ms elapsed — callback fired
 * wheel.cancel(id); // already fired, returns false
 * @endcode
 */
class TimerWheel {
public:
  /** @brief Timer callback type. */
  using Callback = std::function<void()>;
  /** @brief Timer identifier type. */
  using TimerId = uint64_t;

  /** @brief Number of timing wheel levels. */
  static constexpr size_t LEVELS = 4;
  /** @brief Number of slots per level. */
  static constexpr size_t SLOTS_PER_LEVEL = 256;
  /** @brief Invalid timer ID sentinel. */
  static constexpr TimerId kInvalid = 0;

  /**
   * @brief Initialize the timer wheel.
   *
   * @param pool_capacity Maximum number of Entry objects in the FixedPoolResource pool.
   *                      Set this higher than the maximum number of simultaneously
   *                      active timers. Default: 4096.
   */
  explicit TimerWheel(size_t pool_capacity = 4096)
      : pool_(pool_capacity) { index_.reserve(pool_capacity); }

  // Copy and move are disabled (pointer-based internal link structure).
  TimerWheel(const TimerWheel &) = delete;
  TimerWheel &operator=(const TimerWheel &) = delete;
  TimerWheel(TimerWheel &&) = delete;
  TimerWheel &operator=(TimerWheel &&) = delete;

  /** @brief Destructor: returns all unexpired timer entries to the pool. */
  ~TimerWheel() { clear(); }

  /**
   * @brief Schedule a callback to fire after the specified delay (ms).
   *
   * Allocates an Entry from FixedPoolResource in O(1).
   * Returns kInvalid if the pool is exhausted.
   *
   * @param delay_ms Delay in milliseconds. 0 fires on the next tick.
   * @param fn       Callback to invoke on expiry.
   * @returns TimerId for use with cancel(). Returns kInvalid on pool exhaustion.
   */
  [[nodiscard]] TimerId schedule(uint64_t delay_ms, Callback fn) {
    void *raw = pool_.allocate();
    if (!raw) [[unlikely]] return kInvalid;

    auto *e = new (raw) Entry{};
    e->fn        = std::move(fn);
    // Guard BEFORE assignment: if wrap-around already brought next_id_ to 0,
    // skip it here so kInvalid (0) is never returned as a valid TimerId.
    if (next_id_ == kInvalid) next_id_ = 1;
    e->id        = next_id_++;
    e->expiry_ms = current_ms_ + delay_ms;

    insert(e);
    ++count_;
    return e->id;
  }

  /**
   * @brief Cancel a timer.
   *
   * Uses the TimerId → Entry* index to remove the entry directly in O(1).
   *
   * @param id TimerId returned by schedule().
   * @returns true if the timer was cancelled before expiry; false if already
   *          fired or not found.
   */
  bool cancel(TimerId id) {
    if (id == kInvalid) return false;
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    Entry *e = it->second;
    index_.erase(it);
    unlink(e->level_, e->slot_, e);
    e->~Entry();
    pool_.deallocate(e);
    --count_;
    return true;
  }

  /**
   * @brief Advance the clock by elapsed_ms and fire all expired callbacks.
   *
   * Processes level 0 every ms; higher levels are processed on cascade conditions.
   *
   * @param elapsed_ms Time elapsed since the last tick (milliseconds).
   * @returns Number of callbacks fired.
   */
  size_t tick(uint64_t elapsed_ms) {
    uint64_t target = current_ms_ + elapsed_ms;
    size_t fired = 0;

    while (current_ms_ < target) {
      ++current_ms_;
      // Process level 0 slot
      size_t slot0 = static_cast<size_t>(current_ms_ & (SLOTS_PER_LEVEL - 1));
      fired += fire_slot(0, slot0);

      // Level cascade: redistribute entries from upper levels to lower ones
      for (size_t lv = 1; lv < LEVELS; ++lv) {
        uint64_t divisor = kSlotMs[lv];
        if ((current_ms_ % divisor) == 0) {
          size_t sl = static_cast<size_t>((current_ms_ / divisor) & (SLOTS_PER_LEVEL - 1));
          cascade(lv, sl);
        } else {
          break;
        }
      }
    }
    return fired;
  }

  /**
   * @brief Return the time remaining until the next expiry (ms), for use as a poll timeout.
   *
   * Use as the timeout value in the Reactor `poll()` call:
   * @code
   * uint64_t timeout = wheel.next_expiry_ms();
   * epoll_wait(epfd, events, N, (int)std::min(timeout, (uint64_t)INT_MAX));
   * @endcode
   *
   * @returns `UINT64_MAX` if no timers are pending; otherwise remaining ms (minimum 0).
   *
   * @warning **Thread safety**: TimerWheel is NOT thread-safe. This method
   *   iterates `index_` without a lock. It MUST be called from the same
   *   thread as `tick()`, `schedule()`, and `cancel()` (i.e., the Reactor
   *   event loop thread). Calling it concurrently with any of those methods
   *   causes a data race (TSan violation).
   */
  [[nodiscard]] uint64_t next_expiry_ms() const noexcept {
    if (count_ == 0) return std::numeric_limits<uint64_t>::max();

    uint64_t earliest = std::numeric_limits<uint64_t>::max();
    for (const auto &[id, e] : index_) {
      if (e->expiry_ms < earliest)
        earliest = e->expiry_ms;
    }
    if (earliest <= current_ms_) return 0;
    return earliest - current_ms_;
  }

  /**
   * @brief Return the current virtual clock value (ms).
   *
   * Accumulated elapsed time since construction via tick() calls.
   * @returns Milliseconds elapsed since construction.
   */
  [[nodiscard]] uint64_t now_ms() const noexcept { return current_ms_; }

  /** @brief Return the number of pending timers. */
  [[nodiscard]] size_t count() const noexcept { return count_; }

private:
  /**
   * @brief Timer entry — doubly linked list node.
   *
   * Created via placement new from FixedPoolResource.
   */
  struct Entry {
    Callback  fn;
    TimerId   id        = kInvalid;
    uint64_t  expiry_ms = 0;
    Entry    *next      = nullptr;
    Entry    *prev      = nullptr;
    uint8_t   level_    = 0;   ///< Current level (for O(1) cancel)
    uint8_t   slot_     = 0;   ///< Current slot  (for O(1) cancel)
  };

  /** @brief Slot resolution per level (ms): 1 ms, 256 ms, 65536 ms, 16777216 ms. */
  static constexpr uint64_t kSlotMs[LEVELS] = {1, 256, 65536, 16777216};

  /**
   * @brief Insert an entry into the appropriate level/slot.
   *
   * Determines the level based on the difference between expiry_ms and current_ms_.
   * @param e Entry pointer to insert.
   */
  void insert(Entry *e) {
    uint64_t delta = (e->expiry_ms > current_ms_) ? (e->expiry_ms - current_ms_) : 0;

    size_t lv = 0;
    for (size_t l = LEVELS - 1; l > 0; --l) {
      if (delta >= kSlotMs[l]) {
        lv = l;
        break;
      }
    }

    size_t sl;
    if (lv == 0) {
      sl = static_cast<size_t>(e->expiry_ms & (SLOTS_PER_LEVEL - 1));
    } else {
      sl = static_cast<size_t>((e->expiry_ms / kSlotMs[lv]) & (SLOTS_PER_LEVEL - 1));
    }

    // Record position for O(1) cancel
    e->level_ = static_cast<uint8_t>(lv);
    e->slot_  = static_cast<uint8_t>(sl);
    index_[e->id] = e;

    // Insert at head
    e->next = slots_[lv][sl];
    e->prev = nullptr;
    if (slots_[lv][sl])
      slots_[lv][sl]->prev = e;
    slots_[lv][sl] = e;
  }

  /**
   * @brief Remove an Entry from a specific level/slot (O(1) via doubly linked list).
   *
   * @param lv  Level the entry belongs to.
   * @param sl  Slot index the entry belongs to.
   * @param e   Entry pointer to remove.
   */
  void unlink(size_t lv, size_t sl, Entry *e) noexcept {
    if (e->prev)
      e->prev->next = e->next;
    else
      slots_[lv][sl] = e->next; // update head
    if (e->next)
      e->next->prev = e->prev;
    e->next = nullptr;
    e->prev = nullptr;
  }

  /**
   * @brief Fire all expired entries in a slot and return the count.
   *
   * Non-expired entries are re-inserted into the correct slot.
   *
   * @param level Level to process.
   * @param slot  Slot index to process.
   * @returns Number of callbacks fired.
   */
  size_t fire_slot(size_t level, size_t slot) {
    size_t fired = 0;
    Entry *e = slots_[level][slot];
    slots_[level][slot] = nullptr;

    while (e) {
      Entry *nxt = e->next;
      e->next = nullptr;
      e->prev = nullptr;

      if (e->expiry_ms <= current_ms_) {
        // Expired — remove from index, fire callback, return to pool
        index_.erase(e->id);
        if (e->fn) e->fn();
        e->~Entry();
        pool_.deallocate(e);
        --count_;
        ++fired;
      } else {
        // Not yet expired — re-insert into the correct slot (insert updates index_)
        insert(e);
      }
      e = nxt;
    }
    return fired;
  }

  /**
   * @brief Cascade entries from an upper level slot down to lower levels.
   *
   * Redistributes entries from the upper level slot into the correct lower-level
   * slots when the timing wheel level transitions.
   *
   * @param level Level to cascade from (must be >= 1).
   * @param slot  Slot index to cascade.
   */
  void cascade(size_t level, size_t slot) {
    Entry *e = slots_[level][slot];
    slots_[level][slot] = nullptr;

    while (e) {
      Entry *nxt = e->next;
      e->next = nullptr;
      e->prev = nullptr;
      insert(e); // re-insert into lower level
      e = nxt;
    }
  }

  /**
   * @brief Return all entries to the pool (used by the destructor).
   */
  void clear() noexcept {
    for (size_t lv = 0; lv < LEVELS; ++lv) {
      for (size_t sl = 0; sl < SLOTS_PER_LEVEL; ++sl) {
        Entry *e = slots_[lv][sl];
        while (e) {
          Entry *nxt = e->next;
          e->~Entry();
          pool_.deallocate(e);
          e = nxt;
        }
        slots_[lv][sl] = nullptr;
      }
    }
    index_.clear();
    count_ = 0;
  }

  /**
   * @brief Entry object pool — O(1) allocation/deallocation backed by FixedPoolResource.
   *
   * Pre-allocates `pool_capacity` slots of `sizeof(Entry)` bytes.
   * Timer entries are recycled without heap fragmentation.
   */
  FixedPoolResource<sizeof(Entry), alignof(Entry)> pool_;

  /** @brief Slot array [level][slot] → doubly linked list head. */
  Entry   *slots_[LEVELS][SLOTS_PER_LEVEL] = {};

  /** @brief TimerId → Entry* index — supports O(1) cancel() and next_expiry_ms(). */
  std::unordered_map<TimerId, Entry *> index_;

  /** @brief Virtual clock accumulated since construction (ms). */
  uint64_t current_ms_ = 0;

  /** @brief Next timer ID to issue. kInvalid (0) is skipped. */
  TimerId  next_id_ = 1;

  /** @brief Total number of timers currently registered in slots. */
  size_t   count_ = 0;

  /**
   * @brief Compute the bit-shift value for slot advancement at each level.
   *
   * Level `l` covers `2^(l*8)` ms units.
   * This static function is provided for documentation purposes.
   *
   * @param level Level index (0 ~ LEVELS-1).
   * @returns Bit shift value for the given level.
   */
  static constexpr uint64_t level_shift(size_t level) noexcept {
    return level * 8; // each level covers 2^8=256 slots
  }
};

} // namespace qbuem

/** @} */ // end of qbuem_timer
