#pragma once

/**
 * @file qbuem/pipeline/stream.hpp
 * @brief 비동기 스트림 — Stream<T>
 * @defgroup qbuem_stream Stream
 * @ingroup qbuem_pipeline
 *
 * Stream<T>는 AsyncChannel<T>을 소비하는 lazy pull-based 스트림입니다.
 * `co_await stream.next()`로 다음 아이템을 가져오고,
 * 채널이 닫히면 `std::nullopt` (EOS)를 반환합니다.
 *
 * ## Rx-style 연산자 (operator| 파이프 문법)
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

/** @brief 스트림 종료 마커. */
struct StreamEnd {};

/**
 * @brief 스트림 아이템: 값 또는 EOS.
 *
 * @tparam T 값 타입.
 */
template <typename T>
using StreamItem = std::variant<T, StreamEnd>;

/**
 * @brief 비동기 스트림 — AsyncChannel<T> 기반 pull 인터페이스.
 *
 * @tparam T 아이템 타입.
 */
template <typename T>
class Stream {
public:
  /**
   * @brief AsyncChannel을 공유 소유로 래핑해 Stream을 생성합니다.
   */
  explicit Stream(std::shared_ptr<AsyncChannel<T>> channel)
      : channel_(std::move(channel)) {}

  /**
   * @brief 다음 아이템을 가져옵니다.
   *
   * 채널이 비었으면 대기, EOS면 `std::nullopt`.
   *
   * @returns 아이템 또는 `std::nullopt` (EOS).
   */
  Task<std::optional<T>> next() {
    co_return co_await channel_->recv();
  }

  /**
   * @brief 스트림을 두 개의 독립 스트림으로 분기합니다 (tee).
   *
   * 내부적으로 원본 채널에서 읽어 두 채널에 복사합니다.
   * 둘 중 느린 소비자가 백프레셔를 유발할 수 있습니다.
   *
   * @param cap 분기 채널 용량.
   * @returns {Stream<T>, Stream<T>} 쌍.
   */
  std::pair<Stream<T>, Stream<T>> tee(size_t cap = 128);

  /**
   * @brief 기반 채널을 반환합니다.
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
 * @brief 새 채널과 연결된 Stream을 생성합니다.
 *
 * @tparam T  아이템 타입.
 * @param cap 채널 용량.
 * @returns {Stream<T>, shared_ptr<AsyncChannel<T>>} — 스트림(소비자) + 채널(생산자)
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
 * @brief map — 각 아이템을 변환합니다.
 *
 * @param fn `(T) -> Task<Result<U>>` 변환 함수.
 */
template <typename Fn>
struct StreamMapOp { Fn fn; };

template <typename Fn>
auto stream_map(Fn fn) { return StreamMapOp<Fn>{std::move(fn)}; }

template <typename T, typename Fn>
auto operator|(Stream<T> stream, StreamMapOp<Fn> op) {
  using U = typename std::invoke_result_t<Fn, T>::value_type::value_type;
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
  pump.detach(); // fire-and-forget; Reactor will drive it
  if (auto *r = Reactor::current())
    r->post([h = pump.handle]() mutable { (void)h; }); // already detached

  return out;
}

/**
 * @brief filter — 조건을 만족하는 아이템만 통과시킵니다.
 *
 * @param pred `(const T&) -> bool` 술어 함수.
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
 * @brief chunk — N개씩 묶어 vector로 만듭니다.
 *
 * BatchAction 입력이나 DB bulk insert에 유용합니다.
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
 * @brief take_while — 조건이 거짓이 되면 스트림을 닫습니다.
 *
 * @param pred `(const T&) -> bool` 종료 조건.
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
 * @brief scan — 상태 유지 누적 변환 (std::accumulate의 스트리밍 버전).
 *
 * @param init  초기 상태값.
 * @param fn    `(State&, T) -> U` 변환 함수.
 */
template <typename State, typename Fn>
struct StreamScanOp { State init; Fn fn; };

template <typename State, typename Fn>
auto stream_scan(State init, Fn fn) {
  return StreamScanOp<State, Fn>{std::move(init), std::move(fn)};
}

/**
 * @brief scan operator| — 상태 누적 변환을 스트림에 적용합니다.
 *
 * @tparam T     입력 아이템 타입.
 * @tparam State 누적 상태 타입.
 * @tparam Fn    `(State&, T) -> U` 변환 함수 타입.
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
// flat_map
// ---------------------------------------------------------------------------

/**
 * @brief flat_map — T를 Stream<U>로 변환 후 평탄화합니다.
 *
 * @param fn `(T) -> Stream<U>` 변환 함수.
 *           각 입력 아이템에 대해 내부 스트림을 생성하고 모든 아이템을 출력합니다.
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
 * @brief zip — 두 스트림을 쌍(pair)으로 묶습니다.
 *
 * 두 스트림 중 하나가 EOS가 되면 출력 스트림도 닫힙니다.
 *
 * @tparam T 첫 번째 스트림 아이템 타입.
 * @tparam U 두 번째 스트림 아이템 타입.
 * @param a   첫 번째 스트림.
 * @param b   두 번째 스트림.
 * @param cap 출력 채널 용량.
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
 * @brief merge — 여러 스트림을 하나로 병합합니다.
 *
 * 모든 입력 스트림이 EOS가 되면 출력 스트림도 닫힙니다.
 * 각 스트림에서 독립적인 펌프 코루틴이 실행되어 먼저 도착한 아이템부터 전달됩니다.
 *
 * @tparam T 아이템 타입.
 * @param streams 병합할 스트림 목록.
 * @param cap     출력 채널 용량.
 * @returns 병합된 `Stream<T>`.
 */
template <typename T>
Stream<T> stream_merge(std::vector<Stream<T>> streams, size_t cap = 256) {
  if (streams.empty()) {
    // 빈 병합: 즉시 닫힌 스트림 반환
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
