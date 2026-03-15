#pragma once

/**
 * @file qbuem/pipeline/stream_ops.hpp
 * @brief Extended Rx-style stream operators — throttle, debounce, tumbling window
 * @defgroup qbuem_stream_ops StreamOps
 * @ingroup qbuem_pipeline
 *
 * This header extends the core stream operators defined in stream.hpp with
 * additional Rx-style combinators:
 *
 * | Operator               | Description                                        |
 * |------------------------|----------------------------------------------------|
 * | `stream_throttle`      | Token-bucket rate limiter (max N items / window)   |
 * | `stream_debounce`      | Emit item only after `gap_ms` silence               |
 * | `stream_tumbling_window` | Group items into fixed-size time windows          |
 *
 * The `flat_map`, `zip`, and `merge` operators are defined in stream.hpp
 * and re-exported here for convenience via the same `qbuem` namespace.
 *
 * ## Usage
 * ```cpp
 * #include <qbuem/pipeline/stream_ops.hpp>
 *
 * auto throttled = my_stream | stream_throttle<MyEvent>(100, 1000); // 100/s
 * auto debounced = my_stream | stream_debounce<MyEvent>(50);        // 50 ms gap
 * auto windowed  = my_stream | stream_tumbling_window<MyEvent>(200); // 200 ms
 * ```
 * @{
 */

#include <qbuem/pipeline/stream.hpp>

#include <chrono>
#include <vector>

namespace qbuem {

// ─── flat_map (re-export) ─────────────────────────────────────────────────────
// StreamFlatMapOp and its operator| are already defined in stream.hpp.
// They are part of the same namespace, so no additional declarations needed.

// ─── zip (re-export) ──────────────────────────────────────────────────────────
// stream_zip<A,B> is already defined in stream.hpp.

// ─── merge (two-stream overload) ──────────────────────────────────────────────

/**
 * @brief Merge two streams of the same type into a single stream.
 *
 * Items from both streams are interleaved in arrival order.
 * The output stream closes when both input streams reach EOS.
 *
 * This is a two-argument convenience wrapper around the variadic
 * `stream_merge(std::vector<Stream<T>>)` defined in stream.hpp.
 *
 * @tparam T  Item type (must be the same for both streams).
 * @param a   First input stream.
 * @param b   Second input stream.
 * @param cap Output channel capacity (default: 256).
 * @returns Merged `Stream<T>`.
 */
template <typename T>
Stream<T> stream_merge(Stream<T> a, Stream<T> b, size_t cap = 256) {
  std::vector<Stream<T>> streams;
  streams.push_back(std::move(a));
  streams.push_back(std::move(b));
  return stream_merge(std::move(streams), cap);
}

// ─── throttle ────────────────────────────────────────────────────────────────

/**
 * @brief Token-bucket throttle operator tag.
 *
 * @tparam T  Stream item type.
 */
template <typename T>
struct StreamThrottleOp {
  uint64_t max_per_ms; ///< Maximum items allowed per `window_ms` milliseconds.
  uint64_t window_ms;  ///< Measurement window size in milliseconds.
};

/**
 * @brief Create a throttle operator.
 *
 * Limits output to `max_per_ms` items per `window_ms` milliseconds using a
 * token-bucket algorithm.  When the bucket is empty the coroutine suspends
 * until the next refill tick.
 *
 * @tparam T          Stream item type.
 * @param max_per_ms  Token bucket capacity and refill amount per window.
 * @param window_ms   Refill interval in milliseconds (default: 1000 ms).
 * @returns `StreamThrottleOp<T>` tag for use with `operator|`.
 */
template <typename T>
auto stream_throttle(uint64_t max_per_ms, uint64_t window_ms = 1000) {
  return StreamThrottleOp<T>{max_per_ms, window_ms};
}

/**
 * @brief Apply token-bucket throttling to a stream.
 *
 * The pump coroutine maintains a token count.  Each forwarded item consumes
 * one token.  Tokens are refilled to `max_per_ms` every `window_ms` ms.
 * When the bucket is empty the coroutine busy-yields until refilled.
 *
 * @tparam T   Item type.
 * @param stream Input stream.
 * @param op     Throttle configuration.
 * @returns Throttled `Stream<T>`.
 */
template <typename T>
Stream<T> operator|(Stream<T> stream, StreamThrottleOp<T> op) {
  auto [out, chan] = make_stream<T>();

  struct Pump {
    static Task<void> run(Stream<T> in,
                          std::shared_ptr<AsyncChannel<T>> out,
                          uint64_t max_tokens,
                          uint64_t window_ms) {
      using Clock = std::chrono::steady_clock;
      using Ms    = std::chrono::milliseconds;

      uint64_t tokens      = max_tokens;
      auto     window_start = Clock::now();

      for (;;) {
        // Refill tokens if the window has elapsed.
        auto now    = Clock::now();
        auto elapsed = std::chrono::duration_cast<Ms>(now - window_start).count();
        if (static_cast<uint64_t>(elapsed) >= window_ms) {
          tokens       = max_tokens;
          window_start = now;
        }

        // If bucket is empty, yield and retry next iteration.
        if (tokens == 0) {
          struct Yield {
            bool await_ready() noexcept { return false; }
            void await_suspend(std::coroutine_handle<> h) noexcept {
              if (auto* r = Reactor::current())
                r->post([h]() mutable { h.resume(); });
              else
                h.resume();
            }
            void await_resume() noexcept {}
          };
          co_await Yield{};
          continue;
        }

        auto item = co_await in.next();
        if (!item) { out->close(); co_return; }

        --tokens;
        co_await out->send(std::move(*item));
      }
    }
  };

  auto pump = Pump::run(std::move(stream), chan, op.max_per_ms, op.window_ms);
  auto h    = pump.handle;
  pump.detach();
  if (auto* r = Reactor::current())
    r->post([h]() mutable { h.resume(); });

  return out;
}

// ─── debounce ────────────────────────────────────────────────────────────────

/**
 * @brief Debounce operator tag.
 *
 * @tparam T  Stream item type.
 */
template <typename T>
struct StreamDebounceOp {
  uint64_t gap_ms; ///< Minimum silence gap (ms) required before emitting an item.
};

/**
 * @brief Create a debounce operator.
 *
 * Emits an item only when no new item has arrived for at least `gap_ms`
 * milliseconds.  Rapid bursts are collapsed to their last item.
 *
 * @tparam T       Stream item type.
 * @param gap_ms   Silence window in milliseconds.
 * @returns `StreamDebounceOp<T>` tag for use with `operator|`.
 */
template <typename T>
auto stream_debounce(uint64_t gap_ms) {
  return StreamDebounceOp<T>{gap_ms};
}

/**
 * @brief Apply debouncing to a stream.
 *
 * The pump coroutine buffers the most recently received item and only
 * forwards it once `gap_ms` have elapsed without a newer item arriving.
 * Uses `std::chrono::steady_clock` for timing; yields to the reactor
 * between poll cycles to avoid busy-spinning.
 *
 * @tparam T   Item type.
 * @param stream Input stream.
 * @param op     Debounce configuration.
 * @returns Debounced `Stream<T>`.
 */
template <typename T>
Stream<T> operator|(Stream<T> stream, StreamDebounceOp<T> op) {
  auto [out, chan] = make_stream<T>();

  struct Pump {
    static Task<void> run(Stream<T> in,
                          std::shared_ptr<AsyncChannel<T>> out,
                          uint64_t gap_ms) {
      using Clock = std::chrono::steady_clock;
      using Ms    = std::chrono::milliseconds;

      std::optional<T>    pending;
      Clock::time_point   last_arrival{};

      for (;;) {
        // Try to pick up a new item without blocking first.
        auto raw_opt = in.channel()->try_recv();
        if (raw_opt) {
          pending      = std::move(*raw_opt);
          last_arrival = Clock::now();
        } else {
          // No new item available; check if it's time to flush.
          if (pending.has_value()) {
            auto elapsed = std::chrono::duration_cast<Ms>(
                               Clock::now() - last_arrival).count();
            if (static_cast<uint64_t>(elapsed) >= gap_ms) {
              co_await out->send(std::move(*pending));
              pending.reset();
            }
          }

          // Check for EOS.
          if (in.channel()->is_closed() && !pending.has_value()) {
            out->close();
            co_return;
          }

          // Yield to avoid busy-spin.
          struct Yield {
            bool await_ready() noexcept { return false; }
            void await_suspend(std::coroutine_handle<> h) noexcept {
              if (auto* r = Reactor::current())
                r->post([h]() mutable { h.resume(); });
              else
                h.resume();
            }
            void await_resume() noexcept {}
          };
          co_await Yield{};
        }
      }
    }
  };

  auto pump = Pump::run(std::move(stream), chan, op.gap_ms);
  auto h    = pump.handle;
  pump.detach();
  if (auto* r = Reactor::current())
    r->post([h]() mutable { h.resume(); });

  return out;
}

// ─── window_tumbling ─────────────────────────────────────────────────────────

/**
 * @brief Tumbling window operator tag.
 *
 * @tparam T  Stream item type.
 */
template <typename T>
struct StreamTumblingWindowOp {
  uint64_t window_ms; ///< Fixed window duration in milliseconds.
};

/**
 * @brief Create a tumbling window operator.
 *
 * Groups items into non-overlapping, fixed-duration time windows.
 * At the end of each window all buffered items are emitted together
 * as a `std::vector<T>`.  Empty windows are not emitted.
 *
 * @tparam T          Stream item type.
 * @param window_ms   Window duration in milliseconds.
 * @returns `StreamTumblingWindowOp<T>` tag for use with `operator|`.
 */
template <typename T>
auto stream_tumbling_window(uint64_t window_ms) {
  return StreamTumblingWindowOp<T>{window_ms};
}

/**
 * @brief Apply a tumbling window to a stream, emitting `vector<T>` per window.
 *
 * Items arriving within the same `window_ms` interval are collected into a
 * vector and forwarded together when the window boundary is crossed.
 * Timing is measured with `std::chrono::steady_clock`.
 *
 * @tparam T   Item type.
 * @param stream Input stream.
 * @param op     Tumbling window configuration.
 * @returns `Stream<std::vector<T>>`.
 */
template <typename T>
Stream<std::vector<T>> operator|(Stream<T> stream, StreamTumblingWindowOp<T> op) {
  auto [out, chan] = make_stream<std::vector<T>>();

  struct Pump {
    static Task<void> run(Stream<T> in,
                          std::shared_ptr<AsyncChannel<std::vector<T>>> out,
                          uint64_t window_ms) {
      using Clock = std::chrono::steady_clock;
      using Ms    = std::chrono::milliseconds;

      std::vector<T>    window_buf;
      Clock::time_point window_start = Clock::now();

      for (;;) {
        // Check if the current window has expired.
        auto now     = Clock::now();
        auto elapsed = std::chrono::duration_cast<Ms>(now - window_start).count();
        if (static_cast<uint64_t>(elapsed) >= window_ms) {
          if (!window_buf.empty()) {
            co_await out->send(std::move(window_buf));
            window_buf.clear();
          }
          window_start = Clock::now();
        }

        // Try to dequeue a new item without blocking.
        auto raw_opt = in.channel()->try_recv();
        if (raw_opt) {
          window_buf.push_back(std::move(*raw_opt));
          continue;
        }

        // EOS: flush remaining items and close.
        if (in.channel()->is_closed()) {
          if (!window_buf.empty())
            co_await out->send(std::move(window_buf));
          out->close();
          co_return;
        }

        // Nothing available — yield to reactor.
        struct Yield {
          bool await_ready() noexcept { return false; }
          void await_suspend(std::coroutine_handle<> h) noexcept {
            if (auto* r = Reactor::current())
              r->post([h]() mutable { h.resume(); });
            else
              h.resume();
          }
          void await_resume() noexcept {}
        };
        co_await Yield{};
      }
    }
  };

  auto pump = Pump::run(std::move(stream), chan, op.window_ms);
  auto h    = pump.handle;
  pump.detach();
  if (auto* r = Reactor::current())
    r->post([h]() mutable { h.resume(); });

  return out;
}

} // namespace qbuem

/** @} */
