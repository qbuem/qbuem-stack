#pragma once

/**
 * @file qbuem/shm/topic_schema_registry.hpp
 * @brief Compile-time and runtime topic schema registry for SHM/MessageBus validation.
 * @defgroup qbuem_shm_schema TopicSchemaRegistry
 * @ingroup qbuem_shm
 *
 * ## Overview
 * `TopicSchemaRegistry` provides automated schema enforcement at the IPC boundary.
 * Topics are registered with a type descriptor (size, alignment, trivially-copyable
 * requirement) and optionally a custom byte-level validator.  Every `publish()` call
 * is validated against the registered schema before the message enters the bus.
 *
 * ## Design Goals
 * - **Zero heap allocation** on the validation hot path — validation state lives in
 *   a fixed-capacity flat array.
 * - **Zero dependency** — no JSON or external schema library required.  Binary
 *   structural validation only; JSON schema integration is an application concern.
 * - **Wait-free reads** — `lookup()` uses a `LockFreeHashMap` keyed on a topic name
 *   hash, so schema lookups do not block any reactor thread.
 * - **Thread-safe registration** — `register_topic()` is called once at startup from
 *   a single thread; subsequent reads from the reactor are wait-free.
 *
 * ## Usage
 * ```cpp
 * struct Order { uint64_t id; double price; uint32_t qty; };
 * static_assert(std::is_trivially_copyable_v<Order>);
 *
 * TopicSchemaRegistry registry;
 * registry.register_topic<Order>("orders");
 *
 * // Optional: custom byte-level validator
 * registry.register_topic<Order>("orders.validated", [](std::span<const std::byte> buf) {
 *     auto& o = *reinterpret_cast<const Order*>(buf.data());
 *     return o.price > 0.0 && o.qty > 0;
 * });
 *
 * // At publish time:
 * if (!registry.validate("orders", qbuem::as_bytes(order))) {
 *     // schema mismatch — reject the message
 * }
 * ```
 *
 * ## Compile-time Topic Identifiers (GlobalTopic)
 * For zero-string-overhead topic selection, use the `GlobalTopic` helper:
 * ```cpp
 * constexpr auto kOrders = GlobalTopic{"orders"};
 * registry.register_topic<Order>(kOrders);
 * bool ok = registry.validate(kOrders, data);
 * ```
 *
 * ## Thread Safety
 * - `register_topic()` must be called before the reactor starts.  Not thread-safe
 *   with concurrent `validate()` calls.
 * - `validate()` and `lookup()` are fully thread-safe and wait-free after
 *   registration is complete.
 */

#include <qbuem/buf/lock_free_hash_map.hpp>
#include <qbuem/common.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <string_view>
#include <type_traits>

namespace qbuem {

// ── GlobalTopic — compile-time topic identifier ───────────────────────────────

/**
 * @brief Compile-time topic name wrapper with precomputed FNV-1a hash.
 *
 * Provides a strongly-typed, zero-overhead topic identifier that avoids
 * runtime string comparisons on the hot validation path.
 *
 * @code
 * constexpr auto kOrders = GlobalTopic{"orders"};
 * constexpr uint64_t h   = kOrders.hash();  // evaluated at compile time
 * @endcode
 */
struct GlobalTopic {
    std::string_view name;

    /** @brief FNV-1a 64-bit hash of the topic name (constexpr). */
    [[nodiscard]] constexpr uint64_t hash() const noexcept {
        uint64_t h = 14695981039346656037ULL;
        for (char c : name) {
            h ^= static_cast<uint8_t>(c);
            h *= 1099511628211ULL;
        }
        // Ensure the hash is non-zero (LockFreeHashMap reserves key 0).
        return h ? h : 1u;
    }
};

// ── SchemaDescriptor ──────────────────────────────────────────────────────────

/**
 * @brief Describes the expected binary layout of a topic's message type.
 */
struct SchemaDescriptor {
    /** Expected message size in bytes.  0 = any size accepted. */
    size_t  message_size{0};
    /** Expected alignment of the message type. */
    size_t  alignment{1};
    /** Whether the type must satisfy `std::is_trivially_copyable`. */
    bool    require_trivially_copyable{true};
    /** Optional byte-level validator.  Called after structural checks. */
    std::function<bool(std::span<const std::byte>)> validator;
};

// ── TopicSchemaRegistry ────────────────────────────────────────────────────────

/**
 * @brief Runtime registry mapping topic-name hashes to schema descriptors.
 *
 * Internally uses a `LockFreeHashMap<uint64_t, uint32_t>` to map the FNV-1a
 * hash of a topic name to an index into a flat `SchemaDescriptor` array.
 * The validator function is stored separately (outside the lock-free map)
 * because `std::function` exceeds 8 bytes.
 *
 * @tparam Capacity Maximum number of registered topics.  Default: 256.
 */
template <size_t Capacity = 256>
class TopicSchemaRegistry {
    static_assert(Capacity >= 1 && Capacity < UINT32_MAX,
                  "Capacity must be in [1, UINT32_MAX)");

public:
    TopicSchemaRegistry() : hash_to_idx_(Capacity * 2) {}

    TopicSchemaRegistry(const TopicSchemaRegistry&)            = delete;
    TopicSchemaRegistry& operator=(const TopicSchemaRegistry&) = delete;

    // ── Registration ─────────────────────────────────────────────────────────

    /**
     * @brief Register a topic with an automatically derived schema.
     *
     * The schema is deduced from `T`:
     * - `message_size`  = `sizeof(T)`
     * - `alignment`     = `alignof(T)`
     * - `require_trivially_copyable` = true (compile-time assertion)
     *
     * @tparam T  Message type.  Must be trivially copyable.
     * @param  topic_name  Topic name string (e.g., "orders").
     * @param  validator   Optional extra validator (called after size/align checks).
     * @returns true on success, false if the registry is full or the hash collides.
     */
    template <typename T>
    bool register_topic(
        std::string_view topic_name,
        std::function<bool(std::span<const std::byte>)> validator = {}) noexcept
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "SHM message types must be trivially copyable (no hidden copies)");

        return register_topic(topic_name, SchemaDescriptor{
            .message_size             = sizeof(T),
            .alignment                = alignof(T),
            .require_trivially_copyable = true,
            .validator                = std::move(validator)
        });
    }

    /**
     * @brief Register a topic with a `GlobalTopic` compile-time identifier.
     *
     * @tparam T  Message type.  Must be trivially copyable.
     * @param  topic  Compile-time topic descriptor.
     * @param  validator  Optional extra validator.
     */
    template <typename T>
    bool register_topic(
        GlobalTopic topic,
        std::function<bool(std::span<const std::byte>)> validator = {}) noexcept
    {
        return register_topic<T>(topic.name, std::move(validator));
    }

    /**
     * @brief Register a topic with a fully custom `SchemaDescriptor`.
     *
     * Use this when the message type is not known at compile time (e.g.,
     * dynamically loaded plugins with binary schemas).
     *
     * @param topic_name Topic name.
     * @param desc       Schema descriptor.
     * @returns true on success.
     */
    [[nodiscard]] bool register_topic(std::string_view topic_name,
                                      SchemaDescriptor desc) noexcept {
        if (count_ >= Capacity) return false;

        const uint64_t h = hash_of(topic_name);
        const uint32_t idx = static_cast<uint32_t>(count_);

        if (!hash_to_idx_.put(h, idx)) return false;  // collision or full

        descriptors_[count_] = std::move(desc);
        ++count_;
        return true;
    }

    // ── Validation ────────────────────────────────────────────────────────────

    /**
     * @brief Validate a raw message buffer against the registered schema.
     *
     * Checks:
     * 1. Topic is registered (hash known).
     * 2. `buf.size() == descriptor.message_size` (unless size == 0).
     * 3. `buf.data()` is aligned to `descriptor.alignment` (pointer check).
     * 4. Custom `validator(buf)` if provided.
     *
     * All checks run on the calling thread without allocation.
     *
     * @param topic_name  Topic to validate against.
     * @param buf         Raw message bytes.
     * @returns `Result<void>` — success or error code.
     */
    [[nodiscard]] Result<void> validate(std::string_view topic_name,
                                        std::span<const std::byte> buf) const noexcept {
        return validate_by_hash(hash_of(topic_name), buf);
    }

    /**
     * @brief Validate using a precomputed `GlobalTopic` hash.
     *
     * Hot path: hash is evaluated at compile time, lookup is wait-free.
     *
     * @param topic  Compile-time topic identifier.
     * @param buf    Raw message bytes.
     */
    [[nodiscard]] Result<void> validate(GlobalTopic topic,
                                        std::span<const std::byte> buf) const noexcept {
        return validate_by_hash(topic.hash(), buf);
    }

    // ── Lookup ────────────────────────────────────────────────────────────────

    /**
     * @brief Look up a schema descriptor by topic name.
     * @returns Pointer to the descriptor, or nullptr if not registered.
     */
    [[nodiscard]] const SchemaDescriptor* lookup(std::string_view topic_name) const noexcept {
        auto r = hash_to_idx_.get(hash_of(topic_name));
        if (!r) return nullptr;
        return &descriptors_[*r];
    }

    /** @brief Number of registered topics. */
    [[nodiscard]] size_t size() const noexcept { return count_; }

    /** @brief Maximum number of topics this registry can hold. */
    [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity; }

private:
    [[nodiscard]] static uint64_t hash_of(std::string_view s) noexcept {
        return GlobalTopic{s}.hash();
    }

    [[nodiscard]] Result<void> validate_by_hash(
        uint64_t h, std::span<const std::byte> buf) const noexcept
    {
        auto r = hash_to_idx_.get(h);
        if (!r) {
            // Topic not registered → reject
            return std::unexpected(
                std::make_error_code(std::errc::no_such_file_or_directory));
        }

        const SchemaDescriptor& desc = descriptors_[*r];

        // Check 1: size
        if (desc.message_size != 0 && buf.size() != desc.message_size) {
            return std::unexpected(
                std::make_error_code(std::errc::message_size));
        }

        // Check 2: alignment
        const auto addr = reinterpret_cast<uintptr_t>(buf.data());
        if (desc.alignment > 1 && (addr % desc.alignment) != 0) {
            return std::unexpected(
                std::make_error_code(std::errc::invalid_argument));
        }

        // Check 3: custom validator
        if (desc.validator && !desc.validator(buf)) {
            return std::unexpected(
                std::make_error_code(std::errc::invalid_argument));
        }

        return {};
    }

    size_t                              count_{0};
    std::array<SchemaDescriptor, Capacity> descriptors_{};
    // Maps topic-name hash → index into descriptors_.
    // LockFreeHashMap: wait-free reads on the hot path.
    LockFreeHashMap<uint64_t, uint32_t> hash_to_idx_;
};

// ── Convenience: as_bytes helper ──────────────────────────────────────────────

/**
 * @brief Reinterpret a trivially-copyable object as a read-only byte span.
 *
 * For use with `TopicSchemaRegistry::validate()`.
 *
 * @code
 * Order order{...};
 * registry.validate("orders", qbuem::as_bytes(order));
 * @endcode
 */
template <typename T>
    requires std::is_trivially_copyable_v<T>
[[nodiscard]] inline std::span<const std::byte> as_bytes(const T& value) noexcept {
    return {reinterpret_cast<const std::byte*>(&value), sizeof(T)};
}

} // namespace qbuem
