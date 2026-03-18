#pragma once

/**
 * @file qbuem/pipeline/spsc_channel.hpp
 * @brief Lock-free SPSC channel — SpscChannel
 * @defgroup qbuem_spsc_channel SpscChannel
 * @ingroup qbuem_pipeline
 *
 * Single-producer/single-consumer (SPSC) lock-free ring-buffer channel.
 * Based on the Lamport queue algorithm, placing head_/tail_ on separate cache lines.
 * Faster than the MPMC AsyncChannel in 1:1 scenarios.
 *
 * ## Implementation characteristics
 * - `head_` (consumer) and `tail_` (producer) are separated with `alignas(64)`
 * - Capacity is automatically rounded up to the next power of two
 * - Async waiting: cross-reactor wake via `Reactor::post`
 *
 * ## Usage example
 * @code
 * SpscChannel<int> chan(1024);
 * chan.try_send(42);          // producer thread
 * auto v = chan.try_recv();   // consumer thread
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace qbuem {

/**
 * @brief Lock-free SPSC (single-producer/single-consumer) channel.
 *
 * Capacity is automatically rounded up to the next power of two at construction.
 *
 * @tparam T Type of values to transmit (must be movable).
 */
template <typename T>
class SpscChannel {
public:
    /**
     * @brief Constructs a channel with the specified capacity.
     *
     * Capacity is automatically rounded up to the next power of two.
     * (e.g., 5 → 8, 9 → 16)
     *
     * @param capacity Desired capacity (rounded up to the next power of two).
     */
    explicit SpscChannel(size_t capacity) {
        size_t cap = next_pow2(std::max(capacity, size_t{2}));
        mask_ = cap - 1;
        buffer_.resize(cap);
    }

    SpscChannel(const SpscChannel&) = delete;
    SpscChannel& operator=(const SpscChannel&) = delete;

    // -------------------------------------------------------------------------
    // Non-blocking try_send / try_recv
    // -------------------------------------------------------------------------

    /**
     * @brief Attempts to send an item to the channel (non-blocking, producer thread).
     *
     * @param item Item to send (const reference).
     * @returns true on success, false if the channel is full or closed.
     */
    bool try_send(const T& item) {
        return try_send_impl(T(item));
    }

    /**
     * @brief Attempts to send an item to the channel (non-blocking, producer thread).
     *
     * @param item Item to send (move).
     * @returns true on success, false if the channel is full or closed.
     */
    bool try_send(T&& item) {
        return try_send_impl(std::move(item));
    }

    /**
     * @brief Attempts to receive an item from the channel (non-blocking, consumer thread).
     *
     * @returns `std::optional<T>` if an item is available, `std::nullopt` otherwise.
     */
    std::optional<T> try_recv() {
        size_t head = head_.v.load(std::memory_order_relaxed);
        size_t tail = tail_.v.load(std::memory_order_acquire);

        if (head == tail)
            return std::nullopt; // empty

        T item = std::move(buffer_[head & mask_]);
        head_.v.store(head + 1, std::memory_order_release);

        // Wake any waiting producer
        wake_waiter();

        return item;
    }

    // -------------------------------------------------------------------------
    // Async send / recv (coroutine)
    // -------------------------------------------------------------------------

    /**
     * @brief Sends an item. co_awaits when the channel is full.
     *
     * @param item Item to send.
     * @returns `Result<void>{}` or `errc::broken_pipe` (if closed).
     */
    Task<Result<void>> send(T item) {
        for (;;) {
            if (closed_.load(std::memory_order_relaxed))
                co_return unexpected(std::make_error_code(std::errc::broken_pipe));

            if (try_send_impl(std::move(item))) {
                co_return Result<void>{};
            }

            // Full — wait until the consumer frees space
            co_await SendAwaiter{this};

            if (closed_.load(std::memory_order_relaxed))
                co_return unexpected(std::make_error_code(std::errc::broken_pipe));
        }
    }

    /**
     * @brief Receives an item. co_awaits when the channel is empty.
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

            // Empty — wait until the producer inserts an item
            co_await RecvAwaiter{this};

            // Re-check (loop again after wake)
        }
    }

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    /**
     * @brief Closes the channel (EOS propagation).
     *
     * After `close()`, `send()` returns an error.
     * `recv()` returns `std::nullopt` after draining remaining items.
     */
    void close() {
        closed_.store(true, std::memory_order_release);
        // Wake any waiting receiver or sender
        wake_waiter();
    }

    /**
     * @brief Returns whether the channel is closed.
     */
    [[nodiscard]] bool is_closed() const noexcept {
        return closed_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Returns the approximate number of items currently in the channel.
     */
    [[nodiscard]] size_t size_approx() const noexcept {
        size_t head = head_.v.load(std::memory_order_relaxed);
        size_t tail = tail_.v.load(std::memory_order_relaxed);
        return tail >= head ? tail - head : 0;
    }

    /**
     * @brief Returns the channel capacity (always a power of two).
     */
    [[nodiscard]] size_t capacity() const noexcept {
        return mask_ + 1;
    }

private:
    // -------------------------------------------------------------------------
    // Cache-line-separated head/tail
    // -------------------------------------------------------------------------

    struct alignas(64) Head { std::atomic<size_t> v{0}; };
    struct alignas(64) Tail { std::atomic<size_t> v{0}; };

    // -------------------------------------------------------------------------
    // Internal try_send implementation
    // -------------------------------------------------------------------------

    bool try_send_impl(T&& item) {
        if (closed_.load(std::memory_order_relaxed))
            return false;

        size_t tail = tail_.v.load(std::memory_order_relaxed);
        size_t head = head_.v.load(std::memory_order_acquire);

        // Check if ring buffer is full
        if (tail - head >= capacity())
            return false;

        buffer_[tail & mask_] = std::move(item);
        tail_.v.store(tail + 1, std::memory_order_release);

        // Wake any waiting receiver
        wake_waiter();
        return true;
    }

    // -------------------------------------------------------------------------
    // Waiter (single waiter — only one can exist at a time in SPSC)
    // -------------------------------------------------------------------------

    void wake_waiter() {
        // Fast path: avoid expensive LOCK XCHG when no waiter is registered.
        if (waiter_.load(std::memory_order_relaxed) == nullptr) return;
        auto h = waiter_.exchange(nullptr, std::memory_order_acq_rel);
        if (!h) return;

        auto* reactor = waiter_reactor_.exchange(nullptr, std::memory_order_acq_rel);
        if (reactor)
            reactor->post([h]() mutable { h.resume(); });
        else
            h.resume();
    }

    // -------------------------------------------------------------------------
    // Send awaiter (producer waits until space becomes available)
    // -------------------------------------------------------------------------
    struct SendAwaiter {
        SpscChannel<T>* chan;

        bool await_ready() const noexcept {
            if (chan->closed_.load(std::memory_order_relaxed))
                return true;
            size_t tail = chan->tail_.v.load(std::memory_order_relaxed);
            size_t head = chan->head_.v.load(std::memory_order_acquire);
            return (tail - head) < chan->capacity();
        }

        bool await_suspend(std::coroutine_handle<> h) {
            chan->waiter_.store(h, std::memory_order_release);
            chan->waiter_reactor_.store(Reactor::current(), std::memory_order_release);

            // Re-check after registering — avoids lost wake
            size_t tail = chan->tail_.v.load(std::memory_order_relaxed);
            size_t head = chan->head_.v.load(std::memory_order_acquire);
            if ((tail - head) < chan->capacity() || chan->closed_.load(std::memory_order_relaxed)) {
                // No longer need to wait — cancel registration
                chan->waiter_.store(nullptr, std::memory_order_release);
                chan->waiter_reactor_.store(nullptr, std::memory_order_release);
                return false;
            }
            return true;
        }

        void await_resume() noexcept {}
    };

    // -------------------------------------------------------------------------
    // Recv awaiter (consumer waits for an item)
    // -------------------------------------------------------------------------
    struct RecvAwaiter {
        SpscChannel<T>* chan;

        bool await_ready() const noexcept {
            if (chan->closed_.load(std::memory_order_relaxed))
                return true;
            size_t head = chan->head_.v.load(std::memory_order_relaxed);
            size_t tail = chan->tail_.v.load(std::memory_order_acquire);
            return head != tail;
        }

        bool await_suspend(std::coroutine_handle<> h) {
            chan->waiter_.store(h, std::memory_order_release);
            chan->waiter_reactor_.store(Reactor::current(), std::memory_order_release);

            // Re-check after registering
            size_t head = chan->head_.v.load(std::memory_order_relaxed);
            size_t tail = chan->tail_.v.load(std::memory_order_acquire);
            if (head != tail || chan->closed_.load(std::memory_order_relaxed)) {
                chan->waiter_.store(nullptr, std::memory_order_release);
                chan->waiter_reactor_.store(nullptr, std::memory_order_release);
                return false;
            }
            return true;
        }

        void await_resume() noexcept {}
    };

    // -------------------------------------------------------------------------
    // Round-up-to-next-power-of-two utility
    // -------------------------------------------------------------------------
    static size_t next_pow2(size_t n) {
        if (n == 0) return 1;
        size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }

    // -------------------------------------------------------------------------
    // Data members
    // -------------------------------------------------------------------------
    size_t        mask_;          ///< capacity - 1 (bitmask)
    std::vector<T> buffer_;       ///< Ring buffer

    Head head_;  ///< Consumer pointer (cache-line separated)
    Tail tail_;  ///< Producer pointer (cache-line separated)

    std::atomic<bool> closed_{false};

    // Single waiter (SPSC — only one can exist at a time)
    std::atomic<std::coroutine_handle<>> waiter_{nullptr};
    std::atomic<Reactor*>                waiter_reactor_{nullptr};
};

} // namespace qbuem

/** @} */
