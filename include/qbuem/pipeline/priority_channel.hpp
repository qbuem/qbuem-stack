#pragma once

/**
 * @file qbuem/pipeline/priority_channel.hpp
 * @brief Priority channel — PriorityChannel, Priority
 * @defgroup qbuem_priority_channel PriorityChannel
 * @ingroup qbuem_pipeline
 *
 * An MPMC async channel with 3 priority levels (High, Normal, Low).
 * Each priority level uses a separate AsyncChannel<T>,
 * and items are dequeued in High -> Normal -> Low order.
 *
 * ## Usage example
 * @code
 * PriorityChannel<int> chan(128);
 * chan.try_send(42, Priority::High);
 * auto item = chan.try_recv(); // High priority first
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/async_channel.hpp>

#include <array>
#include <atomic>
#include <memory>
#include <optional>

namespace qbuem {

/**
 * @brief Priority enumeration for channel items.
 *
 * - High   (0): Highest priority — always processed first.
 * - Normal (1): Normal priority.
 * - Low    (2): Lowest priority.
 */
enum class Priority { High = 0, Normal = 1, Low = 2 };

/**
 * @brief MPMC channel with 3 priority levels.
 *
 * Uses a separate `AsyncChannel<T>` for each priority level.
 * `try_recv()` / `recv()` check in High -> Normal -> Low order.
 *
 * @tparam T Type of values to transmit (must be movable).
 */
template <typename T>
class PriorityChannel {
public:
    /**
     * @brief Construct a channel with the specified capacity per priority level.
     *
     * @param cap_per_level Capacity of each level's channel (default 128).
     */
    explicit PriorityChannel(size_t cap_per_level = 128) {
        for (auto& ch : channels_)
            ch = std::make_unique<AsyncChannel<T>>(cap_per_level);
    }

    // -------------------------------------------------------------------------
    // Non-blocking send / recv
    // -------------------------------------------------------------------------

    /**
     * @brief Send an item with the specified priority (non-blocking).
     *
     * @param item Item to send.
     * @param prio Priority (default Normal).
     * @returns true on success; false if the channel is full or closed.
     */
    bool try_send(T item, Priority prio = Priority::Normal) {
        if (closed_.load(std::memory_order_relaxed))
            return false;
        return channels_[static_cast<size_t>(prio)]->try_send(std::move(item));
    }

    /**
     * @brief Try to dequeue an item in High -> Normal -> Low order (non-blocking).
     *
     * @returns `std::optional<T>` if an item is available, `std::nullopt` otherwise.
     */
    std::optional<T> try_recv() {
        for (auto& ch : channels_) {
            auto item = ch->try_recv();
            if (item)
                return item;
        }
        return std::nullopt;
    }

    // -------------------------------------------------------------------------
    // Async send / recv (coroutine)
    // -------------------------------------------------------------------------

    /**
     * @brief Send an item with the specified priority. Waits if the channel is full.
     *
     * @param item Item to send.
     * @param prio Priority (default Normal).
     * @returns `Result<void>{}` or an error (`errc::broken_pipe`).
     */
    Task<Result<void>> send(T item, Priority prio = Priority::Normal) {
        if (closed_.load(std::memory_order_relaxed))
            co_return unexpected(std::make_error_code(std::errc::broken_pipe));
        co_return co_await channels_[static_cast<size_t>(prio)]->send(std::move(item));
    }

    /**
     * @brief Receive an item in High → Normal → Low order.
     *
     * Waits on the High level channel if all levels are empty.
     * Returns `std::nullopt` (EOS) once the channel is closed and all levels are drained.
     *
     * @returns An item or `std::nullopt` (EOS).
     */
    Task<std::optional<T>> recv() {
        for (;;) {
            // Non-blocking attempt in High → Normal → Low order
            auto item = try_recv();
            if (item)
                co_return item;

            // All closed and empty — EOS
            if (closed_.load(std::memory_order_acquire)) {
                // Check once more to handle items that arrived just before close
                item = try_recv();
                if (item)
                    co_return item;
                co_return std::nullopt;
            }

            // Wait on the High level channel (wakes immediately when High item arrives)
            auto high_item = co_await channels_[0]->recv();
            if (high_item)
                co_return high_item;

            // High channel returned EOS — drain remaining levels
            for (size_t i = 1; i < channels_.size(); ++i) {
                auto remaining = channels_[i]->try_recv();
                if (remaining)
                    co_return remaining;
            }

            co_return std::nullopt;
        }
    }

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    /**
     * @brief Close all level channels (propagate EOS).
     */
    void close() {
        closed_.store(true, std::memory_order_release);
        for (auto& ch : channels_)
            ch->close();
    }

    /**
     * @brief Check whether the channel is closed.
     */
    [[nodiscard]] bool is_closed() const noexcept {
        return closed_.load(std::memory_order_relaxed);
    }

    // -------------------------------------------------------------------------
    // Statistics
    // -------------------------------------------------------------------------

    /**
     * @brief Return the approximate total item count across all levels.
     */
    [[nodiscard]] size_t size_approx() const noexcept {
        size_t total = 0;
        for (const auto& ch : channels_)
            total += ch->size_approx();
        return total;
    }

    /**
     * @brief Return the approximate item count for a specific priority level.
     *
     * @param p Priority level.
     */
    [[nodiscard]] size_t size_approx(Priority p) const noexcept {
        return channels_[static_cast<size_t>(p)]->size_approx();
    }

private:
    /// AsyncChannel for each priority level (0=High, 1=Normal, 2=Low)
    std::array<std::unique_ptr<AsyncChannel<T>>, 3> channels_;
    std::atomic<bool> closed_{false};
};

} // namespace qbuem

/** @} */
