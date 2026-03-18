#pragma once

/**
 * @file qbuem/pipeline/context.hpp
 * @brief Pipeline item context — immutable persistent linked-list
 * @defgroup qbuem_pipeline_context Context
 * @ingroup qbuem_pipeline
 *
 * Context is an immutable type that carries metadata for pipeline items.
 * It is stored in coroutine frames and remains valid across co_await boundaries.
 *
 * ## Key properties
 * - **Immutable**: `put()` returns a new Context; the original is unchanged
 * - **O(1) copy**: only the shared_ptr head is copied
 * - **O(1) put()**: prepends a new node at the head (linked-list prepend)
 * - **Type-indexed**: slot lookup is based on `std::type_index`
 * - **Coroutine-safe**: stored in the frame rather than thread_local, so valid after thread switches
 *
 * ## Usage example
 * @code
 * Context ctx;
 * ctx = ctx.put(TraceCtx{...});
 * ctx = ctx.put(RequestId{"abc-123"});
 *
 * auto trace = ctx.get<TraceCtx>();  // std::optional<TraceCtx>
 * auto rid   = ctx.get_ptr<RequestId>(); // const RequestId* (no copy)
 * @endcode
 * @{
 */

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <typeindex>
#include <vector>

namespace qbuem {

// ---------------------------------------------------------------------------
// Built-in Context slot type declarations (defined in pipeline/trace_context.hpp)
// ---------------------------------------------------------------------------
struct TraceCtx;      ///< W3C TraceContext (trace_id, span_id, flags)
struct RequestId;     ///< Unique HTTP request ID
struct AuthSubject;   ///< Authenticated user identifier
struct AuthRoles;     ///< Authenticated user role list
struct Deadline;      ///< Request processing deadline timestamp
struct ActiveSpan;    ///< Currently active tracing Span
struct EventTime;     ///< Original event occurrence timestamp (for windowing)
struct SagaId;        ///< Saga distributed transaction ID
struct IdempotencyKey; ///< Idempotency key

/**
 * @brief Pipeline item metadata container (immutable persistent linked-list).
 *
 * ### Ownership rules
 * Context is a value type. A Context derived via `put()` shares nodes with the original.
 * It is safe for multiple Actions to own the same Context during fan-out.
 *
 * ### Storage pattern
 * ```
 * [head] → TraceCtx → RequestId → AuthSubject → nullptr
 * ```
 * Calling `get<T>()` performs a linear scan of the linked list.
 * Fewer slots means faster lookup (typically fewer than 5).
 */
class Context {
public:
  Context() noexcept = default;

  /**
   * @brief Returns a new Context with an additional slot.
   *
   * If a slot of the same type already exists, it is shadowed at the head by the new value.
   * (shadowing — the original node is preserved)
   *
   * @tparam T Type of the value to store.
   * @param  value Value to copy or move in.
   * @returns Context with the new slot added.
   */
  template <typename T>
  [[nodiscard]] Context put(T value) const {
    auto node = std::make_shared<Node>();
    node->type_key = std::type_index(typeid(T));
    node->value    = std::make_shared<T>(std::move(value));
    node->next     = head_;
    Context result;
    result.head_   = std::move(node);
    return result;
  }

  /**
   * @brief Returns a copy of the slot value.
   *
   * @tparam T Type to look up.
   * @returns `std::optional<T>` if a value exists, `std::nullopt` otherwise.
   */
  template <typename T>
  [[nodiscard]] std::optional<T> get() const noexcept {
    const T *ptr = get_ptr<T>();
    if (!ptr)
      return std::nullopt;
    return *ptr;
  }

  /**
   * @brief Returns a pointer to the slot value (no copy).
   *
   * @tparam T Type to look up.
   * @returns `const T*` if the slot exists, `nullptr` otherwise.
   */
  template <typename T>
  [[nodiscard]] const T *get_ptr() const noexcept {
    const std::type_index key(typeid(T));
    // Fast path: check inline cache (up to 4 most-recently-looked-up types).
    for (uint8_t i = 0; i < cache_size_; ++i) {
      if (cache_[i].key == key)
        return static_cast<const T *>(cache_[i].ptr);
    }
    // Slow path: linear scan of linked list.
    for (const Node *n = head_.get(); n; n = n->next.get()) {
      if (n->type_key == key) {
        const void *ptr = n->value.get();
        // Populate cache (evict oldest entry when full via FIFO shift).
        if (cache_size_ < kCacheCapacity) {
          cache_[cache_size_++] = {key, ptr};
        } else {
          for (uint8_t i = 0; i + 1 < kCacheCapacity; ++i)
            cache_[i] = cache_[i + 1];
          cache_[kCacheCapacity - 1] = {key, ptr};
        }
        return static_cast<const T *>(ptr);
      }
    }
    return nullptr;
  }

  /**
   * @brief Returns true if the Context is empty.
   */
  [[nodiscard]] bool empty() const noexcept { return !head_; }

private:
  struct Node {
    std::type_index              type_key{typeid(void)};
    std::shared_ptr<void>        value;
    std::shared_ptr<const Node>  next;
  };

  std::shared_ptr<const Node> head_;

  // Inline lookup cache: stores up to 4 most-recently-accessed (type, ptr) pairs.
  // Mutable so const get_ptr<T>() can populate it.
  // std::type_index has no default constructor; initialize slots with typeid(void).
  struct CacheEntry { std::type_index key{typeid(void)}; const void *ptr = nullptr; };
  static constexpr uint8_t kCacheCapacity = 4;
  mutable std::array<CacheEntry, kCacheCapacity> cache_{};
  mutable uint8_t cache_size_ = 0;
};

// ---------------------------------------------------------------------------
// Built-in slot definitions
// ---------------------------------------------------------------------------

/** @brief W3C Trace Context (W3C traceparent standard). */
struct TraceCtx {
  uint8_t trace_id[16]{};  ///< 128-bit trace identifier
  uint8_t span_id[8]{};    ///< 64-bit span identifier
  uint8_t flags = 0;       ///< trace-flags (bit 0 = sampled)
};

/** @brief Unique HTTP request ID (UUID v4 format recommended). */
struct RequestId {
  std::string value;
};

/** @brief Authenticated user identifier. */
struct AuthSubject {
  std::string value;
};

/** @brief Authenticated user role list. */
struct AuthRoles {
  std::vector<std::string> values;
};

/** @brief Processing deadline. A cancellation signal is sent via stop_token when exceeded. */
struct Deadline {
  std::chrono::steady_clock::time_point at;
};

/** @brief Active tracing Span ID (used for creating child spans). */
struct ActiveSpan {
  uint8_t span_id[8]{};
};

/** @brief Original event occurrence timestamp (used for windowing and watermarking). */
struct EventTime {
  std::chrono::system_clock::time_point at;
};

/** @brief Saga distributed transaction identifier. */
struct SagaId {
  std::string value;
};

/** @brief Idempotency key — used to prevent duplicate processing. */
struct IdempotencyKey {
  std::string value;
};

} // namespace qbuem

/** @} */
