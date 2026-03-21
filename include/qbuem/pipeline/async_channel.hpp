#pragma once

/**
 * @file qbuem/pipeline/async_channel.hpp
 * @brief MPMC async channel — Dmitry Vyukov ring buffer
 * @defgroup qbuem_async_channel AsyncChannel
 * @ingroup qbuem_pipeline
 *
 * AsyncChannel<T> is a multiple-producer/multiple-consumer async channel.
 *
 * ## Implementation
 * Based on Dmitry Vyukov's MPMC ring buffer algorithm.
 * - `head_`/`tail_` cache-line separation (`alignas(64)`)
 * - `send()` / `recv()` — backpressure: co_await when full/empty
 * - `try_send()` / `try_recv()` — wait-free, non-blocking
 * - `close()` + EOS propagation
 * - Cross-reactor wakeup: `waiter.reactor->post([h]{h.resume();})`
 *
 * ## MPMC memory ordering (Dmitry Vyukov specification)
 * - send: `sequence.load(acquire)` → CAS(relaxed,relaxed) → data write
 *         → `sequence.store(tail+1, release)`
 * - recv: `sequence.load(acquire)` → data read
 *         → `sequence.store(head+capacity, release)`
 *
 * ## Usage example
 * @code
 * AsyncChannel<int> chan(1024);
 *
 * // Producer (another coroutine):
 * co_await chan.send(42);
 *
 * // Consumer:
 * auto item = co_await chan.recv(); // std::optional<int>
 * if (!item) { // EOS
 * }
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace qbuem {

/**
 * @brief MPMC async channel.
 *
 * @tparam T Type of value to transmit (must be movable).
 */
template <typename T>
class AsyncChannel {
public:
  /**
   * @brief Creates the channel with the specified capacity.
   *
   * @param capacity Ring buffer capacity. Power of two recommended (rounded up internally).
   */
  explicit AsyncChannel(size_t capacity)
      : capacity_(next_pow2(std::max(capacity, size_t{2}))) {
    slots_ = new Slot[capacity_];
    for (size_t i = 0; i < capacity_; ++i)
      slots_[i].sequence.store(i, std::memory_order_relaxed);
  }

  ~AsyncChannel() { delete[] slots_; }

  AsyncChannel(const AsyncChannel &) = delete;
  AsyncChannel &operator=(const AsyncChannel &) = delete;

  // -------------------------------------------------------------------------
  // try_send / try_recv (lock-free, wait-free)
  // -------------------------------------------------------------------------

  /**
   * @brief Attempts to push an item into the channel (non-blocking).
   *
   * @returns `true` on success, `false` if the ring is full or the channel is closed.
   */
  bool try_send(T value) {
    if (closed_.load(std::memory_order_relaxed)) [[unlikely]]
      return false;

    size_t pos = tail_.load(std::memory_order_relaxed);
    for (;;) {
      Slot &slot = slots_[pos & (capacity_ - 1)];
      size_t seq = slot.sequence.load(std::memory_order_acquire);
      intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

      if (diff == 0) [[likely]] {
        // Slot is free: try to claim it
        if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed,
                                        std::memory_order_relaxed)) [[likely]] {
          slot.storage = std::move(value);
          slot.sequence.store(pos + 1, std::memory_order_release);
          // Inline fast-path: skip function call when no waiters (common case).
          if (recv_waiter_count_.load(std::memory_order_relaxed) != 0) [[unlikely]]
            wake_one_receiver_slow();
          return true;
        }
      } else if (diff < 0) [[unlikely]] {
        return false; // ring full
      } else {
        pos = tail_.load(std::memory_order_relaxed);
      }
    }
  }

  /**
   * @brief Attempts to pop an item from the channel (non-blocking).
   *
   * @returns `std::optional<T>` if an item is available, `std::nullopt` if empty.
   *          `std::nullopt` on a closed empty channel (EOS).
   */
  std::optional<T> try_recv() {
    size_t pos = head_.load(std::memory_order_relaxed);
    for (;;) {
      Slot &slot = slots_[pos & (capacity_ - 1)];
      size_t seq = slot.sequence.load(std::memory_order_acquire);
      intptr_t diff =
          static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

      if (diff == 0) [[likely]] {
        // Slot has data: try to claim it
        if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed,
                                         std::memory_order_relaxed)) [[likely]] {
          T value = std::move(slot.storage);
          slot.sequence.store(pos + capacity_, std::memory_order_release);
          // Inline fast-path: skip function call when no waiters (common case).
          if (send_waiter_count_.load(std::memory_order_relaxed) != 0) [[unlikely]]
            wake_one_sender_slow();
          return value;
        }
      } else if (diff < 0) [[unlikely]] {
        return std::nullopt; // ring empty
      } else {
        pos = head_.load(std::memory_order_relaxed);
      }
    }
  }

  // -------------------------------------------------------------------------
  // Async send / recv (backpressure via co_await)
  // -------------------------------------------------------------------------

  /**
   * @brief Sends an item. co_awaits if the channel is full.
   *
   * Returns an error immediately if the channel is closed.
   *
   * @returns `Result<void>{}` or `errc::broken_pipe` (channel closed).
   */
  Task<Result<void>> send(T value) {
    for (;;) {
      if (closed_.load(std::memory_order_relaxed))
        co_return std::unexpected(std::make_error_code(std::errc::broken_pipe));

      if (try_send_inner(value)) {
        wake_one_receiver();
        co_return Result<void>{};
      }

      // Channel full — wait until a consumer frees space
      co_await SendAwaiter{.chan = this, .waiter = {}};
    }
  }

  /**
   * @brief Receives an item. co_awaits if the channel is empty.
   *
   * Returns `std::nullopt` (EOS) when the channel is closed and empty.
   *
   * @returns An item or `std::nullopt` (EOS).
   */
  Task<std::optional<T>> recv() {
    for (;;) {
      auto item = try_recv();
      if (item)
        co_return item;

      if (closed_.load(std::memory_order_acquire))
        co_return std::nullopt; // EOS

      // Channel empty — wait for an item to arrive
      co_await RecvAwaiter{.chan = this, .waiter = {}};
    }
  }

  // -------------------------------------------------------------------------
  // Batch operations
  // -------------------------------------------------------------------------

  /**
   * @brief Pops up to `max_n` items in a batch (non-blocking).
   *
   * @param out   Buffer to fill with results.
   * @param max_n Maximum number of items to pop (0 means out.size()).
   * @returns Actual number of items popped.
   */
  size_t try_recv_batch(std::span<T> out, size_t max_n = 0) {
    if (max_n == 0 || max_n > out.size())
      max_n = out.size();
    size_t n = 0;
    while (n < max_n) {
      auto item = try_recv();
      if (!item)
        break;
      out[n++] = std::move(*item);
    }
    return n;
  }

  /**
   * @brief Sends multiple items in a batch (non-blocking).
   *
   * @param items Items to send.
   * @returns Actual number of items sent.
   */
  size_t send_batch(std::span<T> items) {
    size_t n = 0;
    for (auto &item : items) {
      if (!try_send(std::move(item)))
        break;
      ++n;
    }
    return n;
  }

  // -------------------------------------------------------------------------
  // Lifecycle
  // -------------------------------------------------------------------------

  /**
   * @brief Closes the channel (EOS propagation).
   *
   * After `close()`, `send()` returns an error.
   * `recv()` returns `std::nullopt` once remaining items are exhausted.
   */
  void close() {
    closed_.store(true, std::memory_order_release);
    wake_all_receivers();
    wake_all_senders();
  }

  /**
   * @brief Returns whether the channel is closed.
   */
  [[nodiscard]] bool is_closed() const noexcept {
    return closed_.load(std::memory_order_relaxed);
  }

  /**
   * @brief Returns the current channel capacity.
   */
  [[nodiscard]] size_t capacity() const noexcept { return capacity_; }

  /**
   * @brief Returns an approximate count of items currently in the channel.
   *
   * @note In a multi-threaded environment this value may change immediately after it is returned.
   */
  [[nodiscard]] size_t size_approx() const noexcept {
    size_t head = head_.load(std::memory_order_relaxed);
    size_t tail = tail_.load(std::memory_order_relaxed);
    return tail >= head ? tail - head : 0;
  }

private:
  // -------------------------------------------------------------------------
  // Ring buffer slot
  // -------------------------------------------------------------------------
  struct alignas(64) Slot {
    std::atomic<size_t> sequence{0};
    T storage;
  };

  // -------------------------------------------------------------------------
  // Awaiter Waiter (send/recv)
  // -------------------------------------------------------------------------
  struct Waiter {
    std::coroutine_handle<> handle;
    Reactor                *reactor;
    Waiter                 *next = nullptr;
  };

  // -------------------------------------------------------------------------
  // Send awaiter (waits for space)
  // -------------------------------------------------------------------------
  struct SendAwaiter {
    AsyncChannel<T> *chan;
    Waiter           waiter;

    [[nodiscard]] bool await_ready() const noexcept {
      return !chan->is_full() || chan->is_closed();
    }

    bool await_suspend(std::coroutine_handle<> h) {
      waiter.handle  = h;
      waiter.reactor = Reactor::current();
      chan->add_send_waiter(&waiter);
      // Re-check: space may have appeared after adding waiter
      if (!chan->is_full() || chan->is_closed()) {
        // If remove returns false, someone already removed us and will resume us
        if (chan->remove_send_waiter(&waiter))
          return false; // we removed ourselves — continue inline
        // else: already removed by wake_one_sender — stay suspended, it will resume us
      }
      return true;
    }

    void await_resume() noexcept {}
  };

  // -------------------------------------------------------------------------
  // Recv awaiter (waits for item)
  // -------------------------------------------------------------------------
  struct RecvAwaiter {
    AsyncChannel<T> *chan;
    Waiter           waiter;

    [[nodiscard]] bool await_ready() const noexcept {
      return !chan->is_empty() || chan->is_closed();
    }

    bool await_suspend(std::coroutine_handle<> h) {
      waiter.handle  = h;
      waiter.reactor = Reactor::current();
      chan->add_recv_waiter(&waiter);
      if (!chan->is_empty() || chan->is_closed()) {
        // If remove returns false, someone already removed us and will resume us
        if (chan->remove_recv_waiter(&waiter))
          return false; // we removed ourselves — continue inline
        // else: already removed by wake_one_receiver — stay suspended, it will resume us
      }
      return true;
    }

    void await_resume() noexcept {}
  };

  // -------------------------------------------------------------------------
  // Internal helpers
  // -------------------------------------------------------------------------

  bool try_send_inner(T &value) {
    size_t pos = tail_.load(std::memory_order_relaxed);
    for (;;) {
      Slot &slot = slots_[pos & (capacity_ - 1)];
      size_t seq = slot.sequence.load(std::memory_order_acquire);
      intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

      if (diff == 0) {
        if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed,
                                        std::memory_order_relaxed)) {
          slot.storage = std::move(value);
          slot.sequence.store(pos + 1, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false;
      } else {
        pos = tail_.load(std::memory_order_relaxed);
      }
    }
  }

  [[nodiscard]] bool is_full() const noexcept {
    size_t tail = tail_.load(std::memory_order_relaxed);
    size_t head = head_.load(std::memory_order_relaxed);
    return (tail - head) >= capacity_;
  }

  [[nodiscard]] bool is_empty() const noexcept {
    size_t tail = tail_.load(std::memory_order_relaxed);
    size_t head = head_.load(std::memory_order_relaxed);
    return tail == head;
  }

  void add_send_waiter(Waiter *w) {
    std::lock_guard lock(send_waiters_mutex_);
    w->next         = send_waiters_;
    send_waiters_   = w;
    send_waiter_count_.fetch_add(1, std::memory_order_relaxed);
  }

  bool remove_send_waiter(Waiter *w) {
    std::lock_guard lock(send_waiters_mutex_);
    Waiter **p = &send_waiters_;
    while (*p) {
      if (*p == w) {
        *p = w->next;
        send_waiter_count_.fetch_sub(1, std::memory_order_relaxed);
        return true;
      }
      p = &(*p)->next;
    }
    return false;
  }

  void add_recv_waiter(Waiter *w) {
    std::lock_guard lock(recv_waiters_mutex_);
    w->next         = recv_waiters_;
    recv_waiters_   = w;
    recv_waiter_count_.fetch_add(1, std::memory_order_relaxed);
  }

  bool remove_recv_waiter(Waiter *w) {
    std::lock_guard lock(recv_waiters_mutex_);
    Waiter **p = &recv_waiters_;
    while (*p) {
      if (*p == w) {
        *p = w->next;
        recv_waiter_count_.fetch_sub(1, std::memory_order_relaxed);
        return true;
      }
      p = &(*p)->next;
    }
    return false;
  }

  // Slow paths (mutex lock) — called only when there are waiters.
  // Marked noinline so the fast path (try_send/try_recv) stays compact.
  [[gnu::noinline]] void wake_one_receiver_slow() noexcept {
    Waiter *w = nullptr;
    {
      std::lock_guard lock(recv_waiters_mutex_);
      if (recv_waiters_) {
        w             = recv_waiters_;
        recv_waiters_ = w->next;
        recv_waiter_count_.fetch_sub(1, std::memory_order_relaxed);
      }
    }
    if (w) resume_waiter(w);
  }

  [[gnu::noinline]] void wake_one_sender_slow() noexcept {
    Waiter *w = nullptr;
    {
      std::lock_guard lock(send_waiters_mutex_);
      if (send_waiters_) {
        w             = send_waiters_;
        send_waiters_ = w->next;
        send_waiter_count_.fetch_sub(1, std::memory_order_relaxed);
      }
    }
    if (w) resume_waiter(w);
  }

  // Called from async send()/recv() coroutine paths (not hot path).
  void wake_one_receiver() {
    if (recv_waiter_count_.load(std::memory_order_relaxed) == 0) return;
    wake_one_receiver_slow();
  }

  void wake_one_sender() {
    if (send_waiter_count_.load(std::memory_order_relaxed) == 0) return;
    wake_one_sender_slow();
  }

  void wake_all_receivers() {
    if (recv_waiter_count_.load(std::memory_order_relaxed) == 0) return;
    Waiter *list = nullptr;
    {
      std::lock_guard lock(recv_waiters_mutex_);
      list           = recv_waiters_;
      recv_waiters_  = nullptr;
      recv_waiter_count_.store(0, std::memory_order_relaxed);
    }
    while (list) {
      Waiter *next = list->next;
      resume_waiter(list);
      list = next;
    }
  }

  void wake_all_senders() {
    if (send_waiter_count_.load(std::memory_order_relaxed) == 0) return;
    Waiter *list = nullptr;
    {
      std::lock_guard lock(send_waiters_mutex_);
      list          = send_waiters_;
      send_waiters_ = nullptr;
      send_waiter_count_.store(0, std::memory_order_relaxed);
    }
    while (list) {
      Waiter *next = list->next;
      resume_waiter(list);
      list = next;
    }
  }

  static void resume_waiter(Waiter *w) {
    // Must resume on the owning Reactor thread — cross-reactor safe
    if (w->reactor)
      w->reactor->post([h = w->handle]() mutable { h.resume(); });
    else
      w->handle.resume();
  }

  static size_t next_pow2(size_t n) {
    if (n == 0) return 1;
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
  }

  // -------------------------------------------------------------------------
  // Data members
  // -------------------------------------------------------------------------
  const size_t capacity_;
  Slot        *slots_;

  // Producer cache line: tail_ + closed_ + recv_waiter_count_
  // try_send reads all three — single cache-line touch on the hot path.
  alignas(64) std::atomic<size_t>   tail_{0};         // producers write
  std::atomic<bool>                 closed_{false};   // co-located with tail_ (no alignas)
  char                              _pad_p_[3]; // NOLINT(modernize-avoid-c-arrays)  // align recv_waiter_count_ to 4B
  std::atomic<uint32_t>             recv_waiter_count_{0}; // producers read in wake_one_receiver

  // Consumer cache line: head_ + send_waiter_count_
  // try_recv reads both — single cache-line touch on the hot path.
  alignas(64) std::atomic<size_t>   head_{0};         // consumers write
  std::atomic<uint32_t>             send_waiter_count_{0}; // consumers read in wake_one_sender
  char                              _pad_c_[64 - sizeof(size_t) - sizeof(uint32_t)]; // NOLINT(modernize-avoid-c-arrays)

  // Waiter lists (cold path only — protected by their respective mutexes)
  std::mutex              send_waiters_mutex_;
  Waiter                 *send_waiters_ = nullptr;

  std::mutex              recv_waiters_mutex_;
  Waiter                 *recv_waiters_ = nullptr;
};

} // namespace qbuem

/** @} */
