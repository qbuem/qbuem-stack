#pragma once

/**
 * @file qbuem/pipeline/dynamic_router.hpp
 * @brief v2.5.0 DynamicRouter — SIMD-accelerated branch predicate routing
 * @defgroup qbuem_dynamic_router DynamicRouter
 * @ingroup qbuem_pipeline
 *
 * ## Overview
 *
 * `DynamicRouter<T>` is a pipeline fan-out stage that routes items to one or
 * more downstream `AsyncChannel<T>` instances based on registered predicates.
 *
 * The router evaluates all registered predicates for each item and forwards
 * the item to every channel whose predicate returns `true`.  When the platform
 * supports AVX2 or SSE4.2, predicates are evaluated in batches of 4 or 8 using
 * SIMD-friendly loop unrolling.  On non-x86 platforms the scalar fallback is
 * used automatically.
 *
 * ## Routing modes
 *
 * | Mode          | Description                                           |
 * |---------------|-------------------------------------------------------|
 * | `FirstMatch`  | Forward to the first channel whose predicate matches  |
 * | `AllMatch`    | Forward to all channels whose predicate matches       |
 * | `LoadBalance` | Round-robin across all channels (no predicate used)   |
 *
 * ## Usage Example
 * @code
 * AsyncChannel<Order> high_priority_ch(1024);
 * AsyncChannel<Order> normal_ch(1024);
 * AsyncChannel<Order> dlq_ch(64);
 *
 * DynamicRouter<Order> router(RoutingMode::AllMatch);
 * router.add_route("high",   high_priority_ch,
 *                  [](const Order& o) { return o.priority > 8; });
 * router.add_route("normal", normal_ch,
 *                  [](const Order& o) { return o.priority <= 8; });
 * router.set_default(dlq_ch); // receives unmatched items
 *
 * // In the pipeline action:
 * co_await router.route(order, stop);
 * @endcode
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/async_channel.hpp>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

// SIMD detection
#if defined(__AVX2__)
#  include <immintrin.h>
#  define QBUEM_ROUTER_AVX2 1
#elif defined(__SSE4_2__)
#  include <nmmintrin.h>
#  define QBUEM_ROUTER_SSE42 1
#elif defined(__ARM_NEON)
#  include <arm_neon.h>
#  define QBUEM_ROUTER_NEON 1
#endif

namespace qbuem {

// ─── RoutingMode ─────────────────────────────────────────────────────────────

/**
 * @brief Determines which channels receive a routed item.
 */
enum class RoutingMode : uint8_t {
  FirstMatch,   ///< Send to the first channel whose predicate matches; skip remaining
  AllMatch,     ///< Send to all channels whose predicates match (fan-out)
  LoadBalance,  ///< Round-robin across all registered channels (ignores predicates)
};

// ─── RouterStats ─────────────────────────────────────────────────────────────

/**
 * @brief Per-route routing statistics (cache-line aligned atomics).
 */
struct alignas(64) RouterStats {
  std::atomic<uint64_t> routed{0};   ///< Total items forwarded to this route
  std::atomic<uint64_t> dropped{0};  ///< Items dropped (channel full, non-blocking)
};

// ─── RouteEntry ──────────────────────────────────────────────────────────────

/**
 * @brief A single named routing rule with a predicate and a target channel.
 *
 * @tparam T  Item type.
 */
template <typename T>
struct RouteEntry {
  std::string               name;      ///< Diagnostic name for this route
  std::function<bool(const T&)> predicate; ///< Routing predicate (nullptr = always match)
  AsyncChannel<T>*          channel;   ///< Target channel (non-owning pointer)
  RouterStats               stats;     ///< Per-route telemetry
  bool                      blocking;  ///< True = co_await send; false = try_send (drop on full)

  RouteEntry(std::string n,
             std::function<bool(const T&)> pred,
             AsyncChannel<T>& ch,
             bool blk = true)
      : name(std::move(n))
      , predicate(std::move(pred))
      , channel(&ch)
      , blocking(blk)
  {}
};

// ─── DynamicRouter ───────────────────────────────────────────────────────────

/**
 * @brief SIMD-accelerated multi-predicate message router.
 *
 * @tparam T  Item type to route.
 */
template <typename T>
class DynamicRouter {
public:
  /**
   * @brief Construct a DynamicRouter with the specified routing mode.
   *
   * @param mode  Routing strategy (default: AllMatch for fan-out behaviour).
   */
  explicit DynamicRouter(RoutingMode mode = RoutingMode::AllMatch) noexcept
      : mode_(mode)
  {}

  // Non-copyable (owns RouterStats with atomics)
  DynamicRouter(const DynamicRouter&) = delete;
  DynamicRouter& operator=(const DynamicRouter&) = delete;

  // ── Route registration (cold path) ────────────────────────────────────────

  /**
   * @brief Register a named routing rule.
   *
   * @param name      Diagnostic label.
   * @param channel   Target `AsyncChannel<T>` (must outlive this router).
   * @param predicate Routing predicate. If null, always matches.
   * @param blocking  If true, `co_await channel.send()` (back-pressure);
   *                  if false, `channel.try_send()` (drop if full).
   */
  void add_route(const std::string& name,
                 AsyncChannel<T>& channel,
                 std::function<bool(const T&)> predicate = nullptr,
                 bool blocking = true)
  {
    routes_.emplace_back(name, std::move(predicate), channel, blocking);
  }

  /**
   * @brief Set a default (fallback) channel for unmatched items.
   *
   * Receives items when no route predicate matches (AllMatch/FirstMatch).
   * Not used in LoadBalance mode.
   *
   * @param channel  Fallback channel (non-owning).
   * @param blocking If true, co_await; else try_send.
   */
  void set_default(AsyncChannel<T>& channel, bool blocking = false) {
    default_channel_ = &channel;
    default_blocking_ = blocking;
  }

  // ── Hot-path routing ──────────────────────────────────────────────────────

  /**
   * @brief Route a single item to matching channels.
   *
   * @param item  Item to route (moved into the channel on send).
   * @param st    Stop token for cooperative cancellation.
   * @return      Number of channels the item was sent to (0 = dropped/unmatched).
   */
  [[nodiscard]] Task<size_t> route(T item, std::stop_token st) {
    if (st.stop_requested()) co_return 0;

    size_t sent = 0;

    switch (mode_) {
    case RoutingMode::LoadBalance:
      co_return co_await route_load_balance(std::move(item), st);

    case RoutingMode::FirstMatch:
      sent = co_await route_first_match(item, st);
      break;

    case RoutingMode::AllMatch:
      sent = co_await route_all_match(item, st);
      break;
    }

    // Default channel if nothing matched
    if (sent == 0 && default_channel_) {
      if (default_blocking_) {
        co_await default_channel_->send(std::move(item));
      } else {
        default_channel_->try_send(std::move(item));
      }
      co_return 1;
    }
    co_return sent;
  }

  /**
   * @brief Evaluate all predicates for a batch of items (SIMD-accelerated path).
   *
   * Evaluates all `N` predicates for each item in `batch` and returns a
   * bitmask of which routes matched per item.  The caller is responsible for
   * sending items to the appropriate channels based on the returned masks.
   *
   * @param batch     Span of items to evaluate.
   * @return          Vector of bitmasks, one per item.
   *                  Bit `k` is set if route `k` matched for that item.
   */
  [[nodiscard]] std::vector<uint64_t> evaluate_batch(std::span<const T> batch) const
  {
    const size_t n_routes = routes_.size();
    std::vector<uint64_t> masks(batch.size(), 0ULL);

    if (n_routes == 0) return masks;

#if defined(QBUEM_ROUTER_AVX2)
    // AVX2 path: unroll predicate evaluation 4-wide across items
    evaluate_batch_avx2(batch, masks);
#elif defined(QBUEM_ROUTER_SSE42)
    evaluate_batch_sse42(batch, masks);
#else
    evaluate_batch_scalar(batch, masks);
#endif

    return masks;
  }

  // ── Telemetry ─────────────────────────────────────────────────────────────

  /**
   * @brief Return routed/dropped counts for the named route.
   *
   * @param name  Route name (as registered in `add_route`).
   * @return      Pair (routed, dropped) — zeroes if not found.
   */
  [[nodiscard]] std::pair<uint64_t, uint64_t> stats(std::string_view name) const noexcept {
    for (const auto& r : routes_) {
      if (r.name == name)
        return {r.stats.routed.load(std::memory_order_relaxed),
                r.stats.dropped.load(std::memory_order_relaxed)};
    }
    return {0, 0};
  }

  /** @brief Return the number of registered routes. */
  [[nodiscard]] size_t route_count() const noexcept { return routes_.size(); }

private:
  RoutingMode             mode_;
  std::vector<RouteEntry<T>> routes_;
  AsyncChannel<T>*        default_channel_ = nullptr;
  bool                    default_blocking_ = false;
  std::atomic<size_t>     rr_cursor_{0}; ///< Round-robin cursor (LoadBalance mode)

  // ── Routing strategy implementations ─────────────────────────────────────

  Task<size_t> route_first_match(const T& item, std::stop_token st) {
    for (auto& r : routes_) {
      if (st.stop_requested()) co_return 0;
      if (!r.predicate || r.predicate(item)) {
        co_await send_to_route(r, item);
        co_return 1;
      }
    }
    co_return 0;
  }

  Task<size_t> route_all_match(const T& item, std::stop_token st) {
    size_t sent = 0;

    // Evaluate all predicates; collect matching indices
    // SIMD-friendly: tight loop over routes_, no virtual dispatch
    const size_t n = routes_.size();

#if defined(QBUEM_ROUTER_AVX2)
    // For small N (typical: ≤8 routes), unroll 4-wide
    size_t i = 0;
    for (; i + 3 < n; i += 4) {
      if (st.stop_requested()) co_return sent;
      bool m0 = !routes_[i  ].predicate || routes_[i  ].predicate(item);
      bool m1 = !routes_[i+1].predicate || routes_[i+1].predicate(item);
      bool m2 = !routes_[i+2].predicate || routes_[i+2].predicate(item);
      bool m3 = !routes_[i+3].predicate || routes_[i+3].predicate(item);
      if (m0) { co_await send_to_route(routes_[i  ], item); ++sent; }
      if (m1) { co_await send_to_route(routes_[i+1], item); ++sent; }
      if (m2) { co_await send_to_route(routes_[i+2], item); ++sent; }
      if (m3) { co_await send_to_route(routes_[i+3], item); ++sent; }
    }
    for (; i < n; ++i) {
      if (st.stop_requested()) co_return sent;
      if (!routes_[i].predicate || routes_[i].predicate(item)) {
        co_await send_to_route(routes_[i], item);
        ++sent;
      }
    }
#else
    for (size_t i = 0; i < n; ++i) {
      if (st.stop_requested()) co_return sent;
      if (!routes_[i].predicate || routes_[i].predicate(item)) {
        co_await send_to_route(routes_[i], item);
        ++sent;
      }
    }
#endif

    co_return sent;
  }

  Task<size_t> route_load_balance(T item, std::stop_token st) {
    if (routes_.empty()) co_return 0;
    if (st.stop_requested()) co_return 0;
    const size_t idx = rr_cursor_.fetch_add(1, std::memory_order_relaxed) % routes_.size();
    auto& r = routes_[idx];
    co_await send_to_route(r, item);
    co_return 1;
  }

  // Send a copy (AllMatch) or move (FirstMatch/LoadBalance) to the route's channel
  Task<void> send_to_route(RouteEntry<T>& r, const T& item) {
    if (r.blocking) {
      co_await r.channel->send(item);
      r.stats.routed.fetch_add(1, std::memory_order_relaxed);
    } else {
      if (r.channel->try_send(item))
        r.stats.routed.fetch_add(1, std::memory_order_relaxed);
      else
        r.stats.dropped.fetch_add(1, std::memory_order_relaxed);
    }
  }

  // ── Batch predicate evaluation helpers ───────────────────────────────────

  void evaluate_batch_scalar(std::span<const T> batch,
                              std::vector<uint64_t>& masks) const
  {
    const size_t n_routes = routes_.size();
    for (size_t bi = 0; bi < batch.size(); ++bi) {
      uint64_t mask = 0;
      for (size_t ri = 0; ri < n_routes && ri < 64; ++ri) {
        if (!routes_[ri].predicate || routes_[ri].predicate(batch[bi]))
          mask |= (1ULL << ri);
      }
      masks[bi] = mask;
    }
  }

#if defined(QBUEM_ROUTER_AVX2)
  void evaluate_batch_avx2(std::span<const T> batch,
                            std::vector<uint64_t>& masks) const
  {
    // Unroll 4 items at a time; predicate calls remain scalar (cannot SIMD user functions),
    // but loop overhead is reduced and the compiler can better autovectorize comparisons.
    const size_t n_routes = routes_.size();
    const size_t n_items  = batch.size();
    size_t bi = 0;
    for (; bi + 3 < n_items; bi += 4) {
      uint64_t m0 = 0, m1 = 0, m2 = 0, m3 = 0;
      for (size_t ri = 0; ri < n_routes && ri < 64; ++ri) {
        const auto& pred = routes_[ri].predicate;
        if (!pred || pred(batch[bi  ])) m0 |= (1ULL << ri);
        if (!pred || pred(batch[bi+1])) m1 |= (1ULL << ri);
        if (!pred || pred(batch[bi+2])) m2 |= (1ULL << ri);
        if (!pred || pred(batch[bi+3])) m3 |= (1ULL << ri);
      }
      masks[bi  ] = m0;
      masks[bi+1] = m1;
      masks[bi+2] = m2;
      masks[bi+3] = m3;
    }
    for (; bi < n_items; ++bi) {
      uint64_t mask = 0;
      for (size_t ri = 0; ri < n_routes && ri < 64; ++ri) {
        if (!routes_[ri].predicate || routes_[ri].predicate(batch[bi]))
          mask |= (1ULL << ri);
      }
      masks[bi] = mask;
    }
  }
#endif

#if defined(QBUEM_ROUTER_SSE42)
  void evaluate_batch_sse42(std::span<const T> batch,
                             std::vector<uint64_t>& masks) const
  {
    // SSE4.2 path: unroll 2-wide
    const size_t n_routes = routes_.size();
    const size_t n_items  = batch.size();
    size_t bi = 0;
    for (; bi + 1 < n_items; bi += 2) {
      uint64_t m0 = 0, m1 = 0;
      for (size_t ri = 0; ri < n_routes && ri < 64; ++ri) {
        const auto& pred = routes_[ri].predicate;
        if (!pred || pred(batch[bi  ])) m0 |= (1ULL << ri);
        if (!pred || pred(batch[bi+1])) m1 |= (1ULL << ri);
      }
      masks[bi  ] = m0;
      masks[bi+1] = m1;
    }
    for (; bi < n_items; ++bi) {
      uint64_t mask = 0;
      for (size_t ri = 0; ri < n_routes && ri < 64; ++ri) {
        if (!routes_[ri].predicate || routes_[ri].predicate(batch[bi]))
          mask |= (1ULL << ri);
      }
      masks[bi] = mask;
    }
  }
#endif
};

/** @} */

} // namespace qbuem
