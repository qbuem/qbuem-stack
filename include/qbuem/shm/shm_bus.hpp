#pragma once

/**
 * @file qbuem/shm/shm_bus.hpp
 * @brief SHMBus — unified intra/inter-process message bus.
 * @defgroup qbuem_shm_bus SHMBus
 * @ingroup qbuem_shm
 *
 * ## Overview
 * `SHMBus` is a unified message bus that abstracts `AsyncChannel<T>`
 * (between threads) and `SHMChannel<T>` (between processes) behind a
 * common API.
 *
 * ## Topic Scope (TopicScope)
 * | Scope | Backend | Latency | Description |
 * |-------|---------|---------|-------------|
 * | `LOCAL_ONLY`   | `AsyncChannel<T>` | ~50ns | Within the same process |
 * | `SYSTEM_WIDE`  | `SHMChannel<T>`   | ~150ns | Cross-process |
 *
 * ## Usage Example
 * @code
 * // Declare topic (once)
 * SHMBus bus;
 * bus.declare<OrderEvent>("trading.orders", TopicScope::SYSTEM_WIDE, 4096);
 *
 * // Publish
 * co_await bus.publish("trading.orders", order);
 *
 * // Subscribe
 * auto sub = bus.subscribe<OrderEvent>("trading.orders");
 * while (auto msg = co_await sub->recv()) {
 *     process(*msg);
 * }
 * @endcode
 *
 * ## Pipeline Integration
 * Can be used as Pipeline Stages via `SHMSource` and `SHMSink`:
 * @code
 * auto pipeline = PipelineBuilder<OrderEvent>()
 *     .source<SHMSource<OrderEvent>>("trading.raw_orders")
 *     .add<NormalizeAction>()
 *     .sink<SHMSink<NormalizedOrder>>("trading.normalized")
 *     .build();
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/async_channel.hpp>
#include <qbuem/shm/shm_channel.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace qbuem::shm {

// ─── TopicScope ───────────────────────────────────────────────────────────────

/** @brief Visibility scope of a topic. */
enum class TopicScope : uint8_t {
    LOCAL_ONLY  = 0, ///< Within the same process (AsyncChannel backend)
    SYSTEM_WIDE = 1, ///< Cross-process (SHMChannel backend)
};

// ─── ISubscription<T> ────────────────────────────────────────────────────────

/**
 * @brief Type-safe subscription handle.
 *
 * Subscriptions are managed via RAII — automatically deregistered on destruction.
 */
template <typename T>
class ISubscription {
public:
    virtual ~ISubscription() = default;

    /**
     * @brief Receives the next message.
     * @returns Message pointer or nullopt (channel closed).
     */
    virtual Task<std::optional<const T*>> recv() = 0;

    /**
     * @brief Non-blocking receive attempt.
     */
    virtual std::optional<const T*> try_recv() = 0;

    /** @brief Subscribed topic name. */
    [[nodiscard]] virtual std::string_view topic() const noexcept = 0;

    /** @brief Subscription scope. */
    [[nodiscard]] virtual TopicScope scope() const noexcept = 0;
};

// ─── TopicDescriptor ─────────────────────────────────────────────────────────

/**
 * @brief Topic metadata (stored internally by SHMBus).
 *
 * Name length is capped at 63 characters to allow stack storage without
 * heap allocation.
 */
struct TopicDescriptor {
    static constexpr size_t kMaxNameLen = 63;

    char       name[kMaxNameLen + 1]{}; // NOLINT(modernize-avoid-c-arrays)
    TopicScope scope{TopicScope::LOCAL_ONLY};
    uint64_t   type_id{0};          ///< Type identifier (sizeof(T) XOR typeid-based)
    size_t     capacity{0};         ///< Ring buffer capacity
    void*      channel_ptr{nullptr};///< AsyncChannel<T>* or SHMChannel<T>* (type-erased)
    std::function<void(void*)> channel_deleter; ///< Type-safe destructor

    /** @brief Sets the name (up to 63 characters). */
    void set_name(std::string_view n) noexcept {
        size_t len = n.size() < kMaxNameLen ? n.size() : kMaxNameLen;
        std::memcpy(name, n.data(), len);
        name[len] = '\0';
    }

    [[nodiscard]] std::string_view get_name() const noexcept { return name; }
};

// ─── SHMBus ───────────────────────────────────────────────────────────────────

/**
 * @brief Unified message bus.
 *
 * ## Internal Structure
 * - Topic registry: fixed-size array + linear scan (up to `kMaxTopics` topics).
 * - Each topic is backed by either `AsyncChannel<T>` or `SHMChannel<T>`.
 * - Subscribers receive messages via per-topic fan-out.
 *
 * @note It is recommended to use one `SHMBus` instance per process.
 */
class SHMBus {
public:
    static constexpr size_t kMaxTopics = 64;

    SHMBus() = default;

    ~SHMBus() {
        size_t n = topic_count_.load(std::memory_order_acquire);
        for (size_t i = 0; i < n; ++i) {
            auto& desc = topics_[i];
            if (desc.channel_ptr != nullptr && desc.channel_deleter) {
                desc.channel_deleter(desc.channel_ptr);
                desc.channel_ptr = nullptr;
            }
        }
    }

    SHMBus(const SHMBus&) = delete;
    SHMBus& operator=(const SHMBus&) = delete;

    // ── Topic Declaration ──────────────────────────────────────────────────

    /**
     * @brief Registers a topic of type `T` on the bus.
     *
     * - `LOCAL_ONLY`: creates an `AsyncChannel<T>`.
     * - `SYSTEM_WIDE`: creates an `SHMChannel<T>` (memfd segment).
     *
     * @tparam T     Message type.
     * @param name   Topic name (e.g. "trading.orders").
     * @param scope  LOCAL_ONLY | SYSTEM_WIDE.
     * @param cap    Ring buffer capacity.
     * @returns true on success, false if already registered or limit exceeded.
     */
    template <typename T>
    bool declare(std::string_view name, TopicScope scope, size_t cap = 1024) {
        static_assert(std::is_trivially_copyable_v<T> || scope == TopicScope::LOCAL_ONLY,
                      "SHMChannel requires trivially_copyable T");
        size_t idx = topic_count_.load(std::memory_order_relaxed);
        if (idx >= kMaxTopics) return false;

        // Duplicate check
        if (find_topic(name) != nullptr) return false;

        auto& desc = topics_[idx];
        desc.set_name(name);
        desc.scope    = scope;
        desc.type_id  = make_type_id<T>();
        desc.capacity = cap;

        if (scope == TopicScope::LOCAL_ONLY) {
            desc.channel_ptr    = new AsyncChannel<T>(cap);
            desc.channel_deleter = [](void* p) {
                delete static_cast<AsyncChannel<T>*>(p);
            };
        } else {
            // SYSTEM_WIDE: create SHMChannel (producer side)
            auto res = SHMChannel<T>::create(name, cap);
            if (!res) return false;
            desc.channel_ptr    = res->release();
            desc.channel_deleter = [](void* p) {
                delete static_cast<SHMChannel<T>*>(p);
            };
        }

        topic_count_.fetch_add(1, std::memory_order_release);
        return true;
    }

    // ── Publish ─────────────────────────────────────────────────────────────

    /**
     * @brief Publishes a message to a topic.
     *
     * @tparam T     Message type.
     * @param name   Topic name.
     * @param msg    Message to send.
     * @returns Success or error (topic not registered, type mismatch, channel error).
     */
    template <typename T>
    Task<Result<void>> publish(std::string_view name, const T& msg) {
        auto* desc = find_topic(name);
        if (!desc) co_return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
        if (desc->type_id != make_type_id<T>()) co_return std::unexpected(std::make_error_code(std::errc::invalid_argument));

        if (desc->scope == TopicScope::LOCAL_ONLY) {
            auto* ch = static_cast<AsyncChannel<T>*>(desc->channel_ptr);
            co_return co_await ch->send(T(msg));
        } else {
            auto* ch = static_cast<SHMChannel<T>*>(desc->channel_ptr);
            co_return co_await ch->send(msg);
        }
    }

    /**
     * @brief Non-blocking publish attempt.
     */
    template <typename T>
    bool try_publish(std::string_view name, const T& msg) {
        auto* desc = find_topic(name);
        if (!desc || desc->type_id != make_type_id<T>()) return false;

        if (desc->scope == TopicScope::LOCAL_ONLY) {
            auto* ch = static_cast<AsyncChannel<T>*>(desc->channel_ptr);
            return ch->try_send(T(msg));
        } else {
            auto* ch = static_cast<SHMChannel<T>*>(desc->channel_ptr);
            return ch->try_send(msg);
        }
    }

    // ── Subscribe ───────────────────────────────────────────────────────────

    /**
     * @brief Subscribes to a topic.
     *
     * @tparam T   Message type.
     * @param name Topic name.
     * @returns Subscription handle or nullptr (topic not registered / type mismatch).
     */
    template <typename T>
    std::unique_ptr<ISubscription<T>> subscribe(std::string_view name) {
        auto* desc = find_topic(name);
        if (!desc || desc->type_id != make_type_id<T>()) return nullptr;

        if (desc->scope == TopicScope::LOCAL_ONLY) {
            auto* ch = static_cast<AsyncChannel<T>*>(desc->channel_ptr);
            return std::make_unique<LocalSub<T>>(ch, name);
        } else {
            auto* ch = static_cast<SHMChannel<T>*>(desc->channel_ptr);
            return std::make_unique<SystemSub<T>>(ch, name);
        }
    }

    // ── Utilities ───────────────────────────────────────────────────────────

    /** @brief Returns the number of registered topics. */
    [[nodiscard]] size_t topic_count() const noexcept {
        return topic_count_.load(std::memory_order_relaxed);
    }

    /** @brief Returns whether a topic is registered. */
    [[nodiscard]] bool has_topic(std::string_view name) const noexcept {
        return find_topic(name) != nullptr;
    }

private:
    // ── Internal subscription implementations ─────────────────────────────

    template <typename T>
    struct LocalSub final : ISubscription<T> {
        AsyncChannel<T>* ch;
        const char*      name_str;
        T                buf_{};  ///< per-subscriber buffer (avoids static thread_local aliasing)

        LocalSub(AsyncChannel<T>* c, std::string_view n)
            : ch(c), name_str(n.data()) {}

        Task<std::optional<const T*>> recv() override {
            auto item = co_await ch->recv();
            if (!item) co_return std::nullopt;
            buf_ = std::move(*item);
            co_return &buf_;
        }

        std::optional<const T*> try_recv() override {
            auto item = ch->try_recv();
            if (!item) return std::nullopt;
            buf_ = std::move(*item);
            return &buf_;
        }

        [[nodiscard]] std::string_view topic() const noexcept override { return name_str; }
        [[nodiscard]] TopicScope scope() const noexcept override { return TopicScope::LOCAL_ONLY; }
    };

    template <typename T>
    struct SystemSub final : ISubscription<T> {
        SHMChannel<T>* ch;
        const char*    name_str;

        SystemSub(SHMChannel<T>* c, std::string_view n)
            : ch(c), name_str(n.data()) {}

        Task<std::optional<const T*>> recv() override { return ch->recv(); }
        std::optional<const T*> try_recv() override { return ch->try_recv(); }
        [[nodiscard]] std::string_view topic() const noexcept override { return name_str; }
        [[nodiscard]] TopicScope scope() const noexcept override { return TopicScope::SYSTEM_WIDE; }
    };

    // ── Topic registry ─────────────────────────────────────────────────────

    [[nodiscard]] TopicDescriptor*       find_topic(std::string_view name) noexcept {
        size_t n = topic_count_.load(std::memory_order_acquire);
        for (size_t i = 0; i < n; ++i) {
            if (topics_[i].get_name() == name) return &topics_[i];
        }
        return nullptr;
    }

    [[nodiscard]] const TopicDescriptor* find_topic(std::string_view name) const noexcept {
        size_t n = topic_count_.load(std::memory_order_acquire);
        for (size_t i = 0; i < n; ++i) {
            if (topics_[i].get_name() == name) return &topics_[i];
        }
        return nullptr;
    }

    /** @brief Generates a type ID (sizeof + alignof based, low collision risk). */
    template <typename T>
    static constexpr uint64_t make_type_id() noexcept {
        return (uint64_t(sizeof(T)) << 32) | uint64_t(alignof(T));
    }

    TopicDescriptor      topics_[kMaxTopics]{}; // NOLINT(modernize-avoid-c-arrays)
    std::atomic<size_t>  topic_count_{0};
};

// ─── Pipeline Integration: SHMSource / SHMSink ───────────────────────────────

/**
 * @brief Pipeline Source action that reads messages from an SHM channel.
 *
 * @tparam T Message type.
 *
 * @code
 * // Consume from an SHM channel at the Pipeline head
 * auto pipeline = PipelineBuilder<MyMsg>()
 *     .source<SHMSource<MyMsg>>("system.ingress")
 *     .add<ProcessAction>()
 *     .build();
 * @endcode
 */
template <typename T>
class SHMSource {
public:
    explicit SHMSource(std::string_view channel_name)
        : name_(channel_name) {}  ///< Copy the string to prevent dangling

    /**
     * @brief Initializes and opens the channel.
     * @returns Success or error.
     */
    Result<void> init() noexcept {
        auto res = SHMChannel<T>::open(name_);
        if (!res) return std::unexpected(res.error());
        channel_ = std::move(*res);
        return Result<void>{};
    }

    /**
     * @brief Asynchronously reads the next message.
     * @returns Message pointer (zero-copy) or nullopt.
     */
    Task<std::optional<const T*>> next() {
        return channel_->recv();
    }

private:
    std::string                   name_;   ///< Owning string (prevents string_view dangling)
    typename SHMChannel<T>::Ptr   channel_;
};

/**
 * @brief Pipeline Sink action that writes messages to an SHM channel.
 *
 * @tparam T Message type.
 *
 * @code
 * auto pipeline = PipelineBuilder<ProcessedMsg>()
 *     .add<TransformAction>()
 *     .sink<SHMSink<ProcessedMsg>>("system.analytics")
 *     .build();
 * @endcode
 */
template <typename T>
class SHMSink {
public:
    explicit SHMSink(std::string_view channel_name, size_t capacity = 1024)
        : name_(channel_name), capacity_(capacity) {}  ///< Copy the string to prevent dangling

    /**
     * @brief Initializes and creates the channel.
     * @returns Success or error.
     */
    Result<void> init() noexcept {
        auto res = SHMChannel<T>::create(name_, capacity_);
        if (!res) return std::unexpected(res.error());
        channel_ = std::move(*res);
        return Result<void>{};
    }

    /**
     * @brief Sends a message to the SHM channel.
     * Applies co_await backpressure when slots are full.
     * @param msg Message to send.
     */
    Task<Result<void>> sink(const T& msg) {
        return channel_->send(msg);
    }

private:
    std::string                   name_;      ///< Owning string (prevents string_view dangling)
    size_t                        capacity_;
    typename SHMChannel<T>::Ptr   channel_;
};

} // namespace qbuem::shm

/** @} */
