#pragma once

/**
 * @file qbuem/pipeline/stream.hpp
 * @brief Asynchronous stream — Stream<T>
 * @defgroup qbuem_stream Stream
 * @ingroup qbuem_pipeline
 *
 * Stream<T> is a lazy pull-based stream that consumes an AsyncChannel<T>.
 * Use `co_await stream.next()` to fetch the next item;
 * returns `std::nullopt` (EOS) when the channel is closed.
 *
 * ## Rx-style operators (operator| pipe syntax)
 * @code
 * auto result = stream
 *     | stream_map([](int x) -> Task<Result<int>> { co_return x * 2; })
 *     | stream_filter([](int x) { return x > 5; })
 *     | stream_chunk(10);
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/async_channel.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace qbuem {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// Result<T> → T unwrapper trait.
/// Used in the stream_map operator to extract U from Fn's return type Task<Result<U>>.
template <typename R>
struct unwrap_result_impl { using type = R; };
template <typename T>
struct unwrap_result_impl<Result<T>> { using type = T; };
template <typename R>
using unwrap_result_t = typename unwrap_result_impl<R>::type;

/** @brief Stream end-of-stream marker. */
struct StreamEnd {};

/**
 * @brief Stream item: a value or EOS.
 *
 * @tparam T Value type.
 */
template <typename T>
using StreamItem = std::variant<T, StreamEnd>;

/**
 * @brief Asynchronous stream — AsyncChannel<T>-based pull interface.
 *
 * @tparam T Item type.
 */
template <typename T>
class Stream {
public:
  /**
   * @brief Construct a Stream by wrapping an AsyncChannel with shared ownership.
   */
  explicit Stream(std::shared_ptr<AsyncChannel<T>> channel)
      : channel_(std::move(channel)) {}

  /**
   * @brief Fetch the next item.
   *
   * Suspends if the channel is empty; returns `std::nullopt` on EOS.
   *
   * @returns Item or `std::nullopt` (EOS).
   */
  Task<std::optional<T>> next() {
    co_return co_await channel_->recv();
  }

  /**
   * @brief Fork the stream into two independent streams (tee).
   *
   * Internally reads from the original channel and copies into two channels.
   * The slower of the two consumers may cause backpressure.
   *
   * @param cap Fork channel capacity.
   * @returns {Stream<T>, Stream<T>} pair.
   */
  std::pair<Stream<T>, Stream<T>> tee(size_t cap = 128);

  /**
   * @brief Return the underlying channel.
   */
  [[nodiscard]] std::shared_ptr<AsyncChannel<T>> channel() const {
    return channel_;
  }

private:
  std::shared_ptr<AsyncChannel<T>> channel_;
};

// ---------------------------------------------------------------------------
// Stream factory helper
// ---------------------------------------------------------------------------

/**
 * @brief Create a Stream backed by a new channel.
 *
 * @tparam T  Item type.
 * @param cap Channel capacity.
 * @returns {Stream<T>, shared_ptr<AsyncChannel<T>>} — stream (consumer) + channel (producer)
 */
template <typename T>
auto make_stream(size_t cap = 256) {
  auto chan   = std::make_shared<AsyncChannel<T>>(cap);
  Stream<T> s = Stream<T>(chan);
  return std::pair{std::move(s), chan};
}

// ---------------------------------------------------------------------------
// Rx-style stream operators
// ---------------------------------------------------------------------------

/**
 * @brief map — transform each item.
 *
 * @param fn `(T) -> Task<Result<U>>` transformation function.
 */
template <typename Fn>
struct StreamMapOp { Fn fn; };

template <typename Fn>
auto stream_map(Fn fn) { return StreamMapOp<Fn>{std::move(fn)}; }

template <typename T, typename Fn>
auto operator|(Stream<T> stream, StreamMapOp<Fn> op) {
  // Extract U from Fn(T) → Task<Result<U>>:
  //   invoke_result_t<Fn, T>         == Task<Result<U>>
  //   Task<Result<U>>::value_type    == Result<U>   (Task coroutine_traits type)
  //   unwrap_result_t<Result<U>>     == U
  using U = unwrap_result_t<typename std::invoke_result_t<Fn, T>::value_type>;
  auto [out, chan] = make_stream<U>();

  // Launch a pump coroutine to transform and forward items.
  // (Caller must ensure a Reactor is running to pump the stream)
  struct Pump {
    static Task<void> run(Stream<T> in, std::shared_ptr<AsyncChannel<U>> out,
                          Fn fn) {
      for (;;) {
        auto item = co_await in.next();
        if (!item) { out->close(); co_return; }
        auto result = co_await fn(std::move(*item));
        if (!result.has_value()) { out->close(); co_return; }
        co_await out->send(std::move(*result));
      }
    }
  };

  auto pump = Pump::run(std::move(stream), chan, std::move(op.fn));
  // Save handle BEFORE detach(): detach() nulls pump.handle.
  // Without this, the lambda below would capture nullptr and never resume
  // the coroutine, leaking the frame at initial_suspend().
  auto h    = pump.handle;
  pump.detach(); // fire-and-forget; Reactor will drive it
  if (auto *r = Reactor::current())
    r->post([h]() mutable { h.resume(); });

  return out;
}

/**
 * @brief filter — pass only items that satisfy the predicate.
 *
 * @param pred `(const T&) -> bool` predicate function.
 */
template <typename Fn>
struct StreamFilterOp { Fn fn; };

template <typename Fn>
auto stream_filter(Fn fn) { return StreamFilterOp<Fn>{std::move(fn)}; }

template <typename T, typename Fn>
Stream<T> operator|(Stream<T> stream, StreamFilterOp<Fn> op) {
  auto [out, chan] = make_stream<T>();

  struct Pump {
    static Task<void> run(Stream<T> in, std::shared_ptr<AsyncChannel<T>> out,
                          Fn pred) {
      for (;;) {
        auto item = co_await in.next();
        if (!item) { out->close(); co_return; }
        if (pred(*item))
          co_await out->send(std::move(*item));
      }
    }
  };

  auto pump = Pump::run(std::move(stream), chan, std::move(op.fn));
  auto h    = pump.handle;
  pump.detach();
  if (auto* r = Reactor::current())
    r->post([h]() mutable { h.resume(); });

  return out;
}

/**
 * @brief chunk — collect N items into a vector.
 *
 * Useful for BatchAction input or DB bulk inserts.
 */
struct StreamChunkOp { size_t n; };
inline StreamChunkOp stream_chunk(size_t n) { return {n}; }

template <typename T>
Stream<std::vector<T>> operator|(Stream<T> stream, StreamChunkOp op) {
  auto [out, chan] = make_stream<std::vector<T>>();

  struct Pump {
    static Task<void> run(Stream<T> in, std::shared_ptr<AsyncChannel<std::vector<T>>> out,
                          size_t n) {
      for (;;) {
        std::vector<T> chunk;
        chunk.reserve(n);
        for (size_t i = 0; i < n; ++i) {
          auto item = co_await in.next();
          if (!item) {
            if (!chunk.empty())
              co_await out->send(std::move(chunk));
            out->close();
            co_return;
          }
          chunk.push_back(std::move(*item));
        }
        co_await out->send(std::move(chunk));
      }
    }
  };

  auto pump = Pump::run(std::move(stream), chan, op.n);
  auto h    = pump.handle;
  pump.detach();
  if (auto *r = Reactor::current())
    r->post([h]() mutable { h.resume(); });

  return out;
}

/**
 * @brief take_while — close the stream when the predicate becomes false.
 *
 * @param pred `(const T&) -> bool` termination condition.
 */
template <typename Fn>
struct StreamTakeWhileOp { Fn fn; };

template <typename Fn>
auto stream_take_while(Fn fn) { return StreamTakeWhileOp<Fn>{std::move(fn)}; }

template <typename T, typename Fn>
Stream<T> operator|(Stream<T> stream, StreamTakeWhileOp<Fn> op) {
  auto [out, chan] = make_stream<T>();

  struct Pump {
    static Task<void> run(Stream<T> in, std::shared_ptr<AsyncChannel<T>> out,
                          Fn pred) {
      for (;;) {
        auto item = co_await in.next();
        if (!item || !pred(*item)) { out->close(); co_return; }
        co_await out->send(std::move(*item));
      }
    }
  };

  auto pump = Pump::run(std::move(stream), chan, std::move(op.fn));
  auto h    = pump.handle;
  pump.detach();
  if (auto *r = Reactor::current())
    r->post([h]() mutable { h.resume(); });

  return out;
}

/**
 * @brief scan — stateful accumulation transform (streaming version of std::accumulate).
 *
 * @param init  Initial state value.
 * @param fn    `(State&, T) -> U` transformation function.
 */
template <typename State, typename Fn>
struct StreamScanOp { State init; Fn fn; };

template <typename State, typename Fn>
auto stream_scan(State init, Fn fn) {
  return StreamScanOp<State, Fn>{std::move(init), std::move(fn)};
}

/**
 * @brief scan operator| — apply stateful accumulation transform to a stream.
 *
 * @tparam T     Input item type.
 * @tparam State Accumulation state type.
 * @tparam Fn    `(State&, T) -> U` transformation function type.
 */
template <typename T, typename State, typename Fn>
auto operator|(Stream<T> stream, StreamScanOp<State, Fn> op) {
  using U = std::invoke_result_t<Fn, State&, T>;
  auto [out, chan] = make_stream<U>();

  struct Pump {
    static Task<void> run(Stream<T> in, std::shared_ptr<AsyncChannel<U>> out,
                          State state, Fn fn) {
      for (;;) {
        auto item = co_await in.next();
        if (!item) { out->close(); co_return; }
        U result = fn(state, std::move(*item));
        co_await out->send(std::move(result));
      }
    }
  };

  auto pump = Pump::run(std::move(stream), chan,
                        std::move(op.init), std::move(op.fn));
  auto h    = pump.handle;
  pump.detach();
  if (auto* r = Reactor::current())
    r->post([h]() mutable { h.resume(); });

  return out;
}

// ---------------------------------------------------------------------------
// Fused map+filter (operator fusion — eliminates an intermediate channel)
// ---------------------------------------------------------------------------

/**
 * @brief Fused map+filter operator.
 *
 * The `stream | stream_map(f) | stream_filter(p)` chain creates two intermediate
 * coroutine frames and channels.  `stream_map_filter` merges them into a single
 * pump, eliminating the intermediate allocations.
 *
 * ### Performance benefits
 * - Eliminates the intermediate `AsyncChannel<U>` → no channel send/recv overhead
 * - Saves one coroutine frame
 *
 * ### Usage example
 * @code
 * // Before (2 intermediate channels):
 * auto s = stream | stream_map(f) | stream_filter(p);
 *
 * // Fused (0 intermediate channels):
 * auto s = stream | stream_map_filter(f, p);
 * @endcode
 *
 * @param map_fn   `(T) -> Task<Result<U>>` transformation function.
 * @param pred     `(const U&) -> bool` filter predicate.
 */
template <typename MapFn, typename FilterFn>
struct StreamMapFilterOp { MapFn map_fn; FilterFn pred; };

template <typename MapFn, typename FilterFn>
auto stream_map_filter(MapFn map_fn, FilterFn pred) {
  return StreamMapFilterOp<MapFn, FilterFn>{std::move(map_fn), std::move(pred)};
}

template <typename T, typename MapFn, typename FilterFn>
auto operator|(Stream<T> stream, StreamMapFilterOp<MapFn, FilterFn> op) {
  using U = typename std::invoke_result_t<MapFn, T>::value_type::value_type;
  auto [out, chan] = make_stream<U>();

  struct Pump {
    static Task<void> run(Stream<T> in, std::shared_ptr<AsyncChannel<U>> out,
                          MapFn map_fn, FilterFn pred) {
      for (;;) {
        auto item = co_await in.next();
        if (!item) { out->close(); co_return; }
        auto result = co_await map_fn(std::move(*item));
        if (!result.has_value()) { out->close(); co_return; }
        // Fused filter: skip channel round-trip for filtered items.
        if (!pred(*result)) continue;
        co_await out->send(std::move(*result));
      }
    }
  };

  auto pump = Pump::run(std::move(stream), chan,
                        std::move(op.map_fn), std::move(op.pred));
  auto h    = pump.handle;
  pump.detach();
  if (auto *r = Reactor::current())
    r->post([h]() mutable { h.resume(); });

  return out;
}

// ---------------------------------------------------------------------------
// flat_map
// ---------------------------------------------------------------------------

/**
 * @brief flat_map — transform each T into a Stream<U> and flatten the results.
 *
 * @param fn `(T) -> Stream<U>` transformation function.
 *           Creates an inner stream for each input item and emits all of its items.
 */
template <typename Fn>
struct StreamFlatMapOp { Fn fn; };

template <typename Fn>
auto stream_flat_map(Fn fn) { return StreamFlatMapOp<Fn>{std::move(fn)}; }

template <typename T, typename Fn>
auto operator|(Stream<T> stream, StreamFlatMapOp<Fn> op) {
  // Determine U from fn(T{}) return type: Stream<U> -> channel() -> AsyncChannel<U>
  // We use decltype on the channel element type.
  using InnerStream = std::invoke_result_t<Fn, T>;
  using ChanPtr     = decltype(std::declval<InnerStream>().channel());
  using U           = typename ChanPtr::element_type::value_type; // AsyncChannel<U>

  auto [out, chan] = make_stream<U>();

  struct Pump {
    static Task<void> run(Stream<T> in, std::shared_ptr<AsyncChannel<U>> out,
                          Fn fn) {
      for (;;) {
        auto item = co_await in.next();
        if (!item) { out->close(); co_return; }

        // Produce inner stream from item
        InnerStream inner = fn(std::move(*item));

        // Drain inner stream into output
        for (;;) {
          auto inner_item = co_await inner.next();
          if (!inner_item) break; // inner EOS — proceed to next outer item
          co_await out->send(std::move(*inner_item));
        }
      }
    }
  };

  auto pump = Pump::run(std::move(stream), chan, std::move(op.fn));
  auto h    = pump.handle;
  pump.detach();
  if (auto* r = Reactor::current())
    r->post([h]() mutable { h.resume(); });

  return out;
}

// ---------------------------------------------------------------------------
// zip
// ---------------------------------------------------------------------------

/**
 * @brief zip — combine two streams into pairs.
 *
 * When either stream reaches EOS the output stream is also closed.
 *
 * @tparam T First stream item type.
 * @tparam U Second stream item type.
 * @param a   First stream.
 * @param b   Second stream.
 * @param cap Output channel capacity.
 * @returns `Stream<std::pair<T, U>>`.
 */
template <typename T, typename U>
Stream<std::pair<T, U>> stream_zip(Stream<T> a, Stream<U> b, size_t cap = 256) {
  auto [out, chan] = make_stream<std::pair<T, U>>(cap);

  struct Pump {
    static Task<void> run(Stream<T> sa, Stream<U> sb,
                          std::shared_ptr<AsyncChannel<std::pair<T, U>>> out) {
      for (;;) {
        auto ia = co_await sa.next();
        auto ib = co_await sb.next();
        if (!ia || !ib) { out->close(); co_return; }
        co_await out->send(std::pair<T, U>{std::move(*ia), std::move(*ib)});
      }
    }
  };

  auto pump = Pump::run(std::move(a), std::move(b), chan);
  auto h    = pump.handle;
  pump.detach();
  if (auto* r = Reactor::current())
    r->post([h]() mutable { h.resume(); });

  return out;
}

// ---------------------------------------------------------------------------
// merge
// ---------------------------------------------------------------------------

/**
 * @brief merge — combine multiple streams into one.
 *
 * The output stream is closed when all input streams reach EOS.
 * An independent pump coroutine runs per stream, forwarding items in arrival order.
 *
 * @tparam T Item type.
 * @param streams List of streams to merge.
 * @param cap     Output channel capacity.
 * @returns Merged `Stream<T>`.
 */
template <typename T>
Stream<T> stream_merge(std::vector<Stream<T>> streams, size_t cap = 256) {
  if (streams.empty()) {
    // Empty merge: return a stream that is immediately closed
    auto [out, chan] = make_stream<T>(2);
    chan->close();
    return out;
  }

  auto out_chan = std::make_shared<AsyncChannel<T>>(cap);
  // atomic counter to track live stream pumps
  auto live = std::make_shared<std::atomic<size_t>>(streams.size());

  struct Pump {
    static Task<void> run(Stream<T> in,
                          std::shared_ptr<AsyncChannel<T>> out,
                          std::shared_ptr<std::atomic<size_t>> live) {
      for (;;) {
        auto item = co_await in.next();
        if (!item) {
          // This stream ended; decrement live count
          size_t remaining = live->fetch_sub(1, std::memory_order_acq_rel) - 1;
          if (remaining == 0)
            out->close();
          co_return;
        }
        co_await out->send(std::move(*item));
      }
    }
  };

  for (auto& s : streams) {
    auto pump = Pump::run(std::move(s), out_chan, live);
    auto h    = pump.handle;
    pump.detach();
    if (auto* r = Reactor::current())
      r->post([h]() mutable { h.resume(); });
  }

  return Stream<T>(out_chan);
}

} // namespace qbuem

/** @} */
