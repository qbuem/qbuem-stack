#pragma once

/**
 * @file qbuem/pipeline/arena_channel.hpp
 * @brief Reactor-local zero-alloc channel — ArenaChannel<T>
 * @defgroup qbuem_arena_channel ArenaChannel
 * @ingroup qbuem_pipeline
 *
 * ArenaChannel<T> is a channel for zero-copy, zero-heap-alloc message passing
 * between producers and consumers within the same reactor.
 *
 * ## Design principles
 *
 * AsyncChannel<T> uses heap slots based on new/delete. For reactor-local
 * communication (between coroutines on the same thread), this allocation is
 * unnecessary. ArenaChannel replaces slot allocation/deallocation with O(1)
 * free-list operations via FixedPoolResource, eliminating malloc cache misses.
 *
 * ### Usage requirements
 * - Safe only between producers and consumers **within the same reactor (thread)**.
 * - Cross-reactor access requires external synchronization.
 *   (For cross-reactor use, use AsyncChannel<T> instead.)
 *
 * ### Memory model
 * ```
 * FixedPoolResource<sizeof(Node)>  ←──────────────────────┐
 *        │  allocate() O(1)                                │ deallocate()
 *        ▼                                                 │
 *   [Node: value + next ptr]──linked list──► head          │
 *        ▲                                                 │
 *   push: placement new into pool slot      pop: call ~T, return slot
 * ```
 *
 * ## Usage example
 * ```cpp
 * ArenaChannel<int> chan(256); // up to 256 slots (heap allocated only once)
 *
 * // Producer within the same reactor:
 * chan.push(42);          // true → success, false → full
 *
 * // Consumer within the same reactor:
 * auto v = chan.pop();    // std::optional<int>
 * if (v) process(*v);
 * ```
 *
 * ## Comparison with AsyncChannel
 * | Property          | AsyncChannel<T>    | ArenaChannel<T>         |
 * |-------------------|--------------------|-------------------------|
 * | Allocation        | new/delete (heap)  | FixedPoolResource (pool)|
 * | Thread safety     | MPMC (atomic CAS)  | single reactor (no lock)|
 * | co_await support  | ✅                 | ❌ (sync only)          |
 * | Use case          | cross-reactor      | within same reactor     |
 * | Allocation cost   | O(1) amortized     | O(1) always             |
 *
 * @{
 */

#include <qbuem/core/arena.hpp>

#include <cstddef>
#include <new>
#include <optional>
#include <type_traits>

namespace qbuem {

/**
 * @brief Reactor-local zero-alloc SPSC/SPMC channel.
 *
 * Internally builds an intrusive linked-list queue on top of FixedPoolResource.
 * All operations are lock-free and safe only within a single reactor.
 *
 * @tparam T Type of value to transmit. Must be MoveConstructible.
 * @tparam Alignment Slot alignment (default 64 — cache-line boundary).
 */
template <typename T, size_t Alignment = 64>
class ArenaChannel {
  // -------------------------------------------------------------------------
  // Internal linked-list node
  // -------------------------------------------------------------------------
  struct alignas(Alignment) Node {
    T       value;
    Node   *next{nullptr};

    template <typename... Args>
    explicit Node(Args&&... args)
        : value(std::forward<Args>(args)...) {}
  };

  static constexpr size_t kNodeSize = sizeof(Node);

public:
  /**
   * @brief Creates the channel with the specified number of slots.
   *
   * At this point, `capacity * sizeof(Node)` bytes are heap-allocated **once**.
   * Subsequent push/pop operations are O(1) from the free-list with no additional heap allocation.
   *
   * @param capacity Maximum number of messages that can be held simultaneously.
   */
  explicit ArenaChannel(size_t capacity)
      : pool_(capacity) {}

  ArenaChannel(const ArenaChannel &) = delete;
  ArenaChannel &operator=(const ArenaChannel &) = delete;
  ArenaChannel(ArenaChannel &&) = delete;
  ArenaChannel &operator=(ArenaChannel &&) = delete;

  ~ArenaChannel() {
    // Drain remaining items to run T destructors
    while (head_) {
      Node *n = head_;
      head_ = n->next;
      n->~Node();
      pool_.deallocate(n);
    }
  }

  // -------------------------------------------------------------------------
  // Producer API
  // -------------------------------------------------------------------------

  /**
   * @brief Pushes a value into the channel (O(1), zero-heap-alloc).
   *
   * @param value Value to transmit (moved in).
   * @returns true  — success.
   * @returns false — pool is full (backpressure).
   */
  bool push(T value) {
    void *slot = pool_.allocate();
    if (!slot) [[unlikely]] return false;

    Node *n = ::new (slot) Node(std::move(value));
    if (tail_) {
      tail_->next = n;
    } else {
      head_ = n;
    }
    tail_ = n;
    ++size_;
    return true;
  }

  /**
   * @brief Constructs a value in-place and pushes it into the channel (O(1)).
   *
   * @tparam Args Constructor argument types for T.
   * @param  args Arguments to forward to T's constructor.
   * @returns true  — success.
   * @returns false — pool is full.
   */
  template <typename... Args>
  bool emplace(Args&&... args) {
    void *slot = pool_.allocate();
    if (!slot) [[unlikely]] return false;

    Node *n = ::new (slot) Node(std::forward<Args>(args)...);
    if (tail_) {
      tail_->next = n;
    } else {
      head_ = n;
    }
    tail_ = n;
    ++size_;
    return true;
  }

  // -------------------------------------------------------------------------
  // Consumer API
  // -------------------------------------------------------------------------

  /**
   * @brief Pops one value from the channel (O(1)).
   *
   * @returns `std::optional<T>` if a value is available, `std::nullopt` if empty.
   */
  std::optional<T> pop() {
    if (!head_) [[unlikely]] return std::nullopt;

    Node *n = head_;
    head_ = n->next;
    if (!head_) tail_ = nullptr;
    --size_;

    std::optional<T> result{std::move(n->value)};
    n->~Node();
    pool_.deallocate(n);
    return result;
  }

  /**
   * @brief Pops up to max_n values from the channel into out (batch dequeue).
   *
   * @param out   Buffer to store results (must support push_back).
   * @param max_n Maximum number of values to pop (0 = unlimited).
   * @returns Actual number of values popped.
   */
  template <typename Container>
  size_t pop_batch(Container &out, size_t max_n = 0) {
    size_t count = 0;
    while (head_ && (max_n == 0 || count < max_n)) {
      Node *n = head_;
      head_ = n->next;
      if (!head_) tail_ = nullptr;
      --size_;

      out.push_back(std::move(n->value));
      n->~Node();
      pool_.deallocate(n);
      ++count;
    }
    return count;
  }

  // -------------------------------------------------------------------------
  // Introspection
  // -------------------------------------------------------------------------

  /// @brief Returns true if the channel is empty.
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

  /// @brief Returns the number of items currently waiting.
  [[nodiscard]] size_t size() const noexcept { return size_; }

  /// @brief Returns the number of remaining slots in the pool.
  [[nodiscard]] size_t available() const noexcept { return pool_.available(); }

  /// @brief Returns the total capacity of the pool.
  [[nodiscard]] size_t capacity() const noexcept { return pool_.capacity(); }

private:
  FixedPoolResource<kNodeSize, Alignment> pool_;
  Node   *head_{nullptr};
  Node   *tail_{nullptr};
  size_t  size_{0};
};

/** @} */

} // namespace qbuem
