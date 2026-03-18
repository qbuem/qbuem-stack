#pragma once

/**
 * @file qbuem/pipeline/windowed_action.hpp
 * @brief v0.9.1 Windowing and event-time processing — WindowedAction, TumblingWindow, SlidingWindow, SessionWindow
 * @defgroup qbuem_windowed_action WindowedAction
 * @ingroup qbuem_pipeline
 *
 * ## Included Components
 *
 * ### EventTime
 * A Context slot type that stores the original event occurrence time (system_clock::time_point).
 * Already declared and defined in context.hpp and reused in this header.
 *
 * ### Watermark
 * A value type representing the progress state of out-of-order event processing.
 * Indicates that all events up to watermark ts have arrived.
 *
 * ### TumblingWindow
 * Fixed-size non-overlapping window. A new window starts every size_ms.
 *
 * ### SlidingWindow
 * Slides every step_ms; each window has size size_ms (step < size).
 *
 * ### SessionWindow
 * Terminates the session when no events arrive for gap_ms or more between events.
 *
 * ### WindowedAction<T, Key, Acc, Out>
 * A key-based time-windowed aggregation action.
 * Emits results when the watermark crosses a window boundary.
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/async_channel.hpp>
#include <qbuem/pipeline/context.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <unordered_map>
#include <vector>

namespace qbuem {

using std::chrono::milliseconds;
using std::chrono::system_clock;

// ============================================================================
// Watermark
// ============================================================================

/**
 * @brief A value type representing the progress state of out-of-order event processing.
 *
 * Guarantees that all events before `ts` have already arrived.
 * When the watermark passes a window boundary, the result of that window is emitted.
 */
struct Watermark {
  system_clock::time_point ts; ///< The reference timestamp up to which all events are guaranteed to have arrived
};

// ============================================================================
// WindowType
// ============================================================================

/**
 * @brief Window type enumeration.
 */
enum class WindowType {
  Tumbling, ///< Fixed-size non-overlapping window
  Sliding,  ///< Sliding window (step < size)
  Session,  ///< Session-based window (gap timeout)
};

// ============================================================================
// Internal window descriptor
// ============================================================================

/**
 * @brief Internal type describing a single time window.
 *
 * Each window covers the interval [start, end).
 */
struct WindowDesc {
  system_clock::time_point start; ///< Window start time (inclusive)
  system_clock::time_point end;   ///< Window end time (exclusive)
};

// ============================================================================
// TumblingWindow
// ============================================================================

/**
 * @brief Fixed-size non-overlapping window policy.
 *
 * Computes the window start time for event time `t`.
 * The window is [floor(t / size) * size, floor(t / size) * size + size).
 */
struct TumblingWindow {
  milliseconds size; ///< Window size

  /**
   * @brief Computes the window that the given event time belongs to.
   *
   * @param event_time The event occurrence time.
   * @returns The window descriptor for the event.
   */
  [[nodiscard]] WindowDesc window_for(system_clock::time_point event_time) const {
    auto epoch_ms = std::chrono::duration_cast<milliseconds>(
        event_time.time_since_epoch()).count();
    auto size_ms = size.count();
    auto bucket  = (epoch_ms / size_ms) * size_ms;
    auto start   = system_clock::time_point(milliseconds(bucket));
    return WindowDesc{start, start + size};
  }

  /**
   * @brief Return the tick interval (tumbling = size).
   */
  [[nodiscard]] milliseconds tick_interval() const { return size; }
};

// ============================================================================
// SlidingWindow
// ============================================================================

/**
 * @brief Sliding window policy.
 *
 * Enumerates all windows that event time `t` could belong to.
 * Each window starts at a multiple of step and has width size.
 *
 * Event `t` belongs to all windows whose start falls in the range (t - size, t].
 */
struct SlidingWindow {
  milliseconds size; ///< Window size
  milliseconds step; ///< Slide interval (step <= size)

  /**
   * @brief Return all windows that the given event time belongs to.
   *
   * @param event_time Event occurrence time.
   * @returns List of all windows containing the event.
   */
  [[nodiscard]] std::vector<WindowDesc> windows_for(
      system_clock::time_point event_time) const {
    auto epoch_ms  = std::chrono::duration_cast<milliseconds>(
        event_time.time_since_epoch()).count();
    auto size_ms   = size.count();
    auto step_ms   = step.count();

    // Earliest window start that can contain this event:
    // start <= event_time < start + size  =>  event_time - size < start <= event_time
    // start is aligned to a multiple of step
    auto first_bucket = ((epoch_ms - size_ms) / step_ms + 1) * step_ms;

    std::vector<WindowDesc> result;
    for (auto bucket_ms = first_bucket; bucket_ms <= epoch_ms; bucket_ms += step_ms) {
      auto start = system_clock::time_point(milliseconds(bucket_ms));
      result.push_back(WindowDesc{start, start + size});
    }
    return result;
  }

  /**
   * @brief Return the tick interval (sliding = step).
   */
  [[nodiscard]] milliseconds tick_interval() const { return step; }
};

// ============================================================================
// SessionWindow
// ============================================================================

/**
 * @brief Session-based window policy.
 *
 * A session ends when no events arrive for gap_ms or more.
 * Session windows are dynamically extended per key.
 */
struct SessionWindow {
  milliseconds gap; ///< Session gap timeout

  /**
   * @brief Return the tick interval (session = gap / 2, minimum 1 ms).
   */
  [[nodiscard]] milliseconds tick_interval() const {
    auto half = gap / 2;
    return half.count() > 0 ? half : milliseconds{1};
  }
};

// ============================================================================
// WindowedAction<T, Key, Acc, Out>
// ============================================================================

/**
 * @brief Key-based time-windowed aggregation action.
 *
 * Manages windows based on event time (EventTime context slot) and watermark.
 * Emits the aggregation result for a window when the watermark crosses its boundary.
 *
 * ### Behavior by window type
 * - **Tumbling**: new window every size_ms; only one window active at a time.
 * - **Sliding**: slides every step_ms; multiple windows active simultaneously.
 * - **Session**: session ends when the gap between events exceeds gap_ms;
 *                independent session management per key.
 *
 * ### Worker coroutine structure
 * 1. **input_worker**: receives items from the input channel and updates window state.
 * 2. **ticker**: advances the watermark every step_ms (or gap/2) and emits completed windows.
 *
 * @tparam T   Input item type.
 * @tparam Key Partitioning key type (uses unordered_map — requires std::hash<Key>).
 * @tparam Acc Aggregation state type.
 * @tparam Out Emitted result type.
 */
template <typename T, typename Key, typename Acc, typename Out>
class WindowedAction {
public:
  /** @brief Key extraction function type: `T → Key`. */
  using KeyFn  = std::function<Key(const T&)>;
  /** @brief Accumulation function type: `(Acc&, T) → void` (folds an item into the accumulator). */
  using AccFn  = std::function<void(Acc&, const T&)>;
  /** @brief Emit function type: `(Key, Acc, window_start) → Out`. */
  using EmitFn = std::function<Out(Key, Acc, system_clock::time_point)>;

  /**
   * @brief WindowedAction configuration struct.
   */
  struct Config {
    WindowType   type    = WindowType::Tumbling; ///< Window type
    milliseconds size    = milliseconds{1000};   ///< Window size (Tumbling/Sliding)
    milliseconds step    = milliseconds{500};    ///< Slide interval (Sliding only)
    milliseconds gap     = milliseconds{5000};   ///< Session gap timeout (Session only)
    KeyFn        key_fn;                         ///< Key extraction function
    AccFn        acc_fn;                         ///< Accumulation function
    EmitFn       emit_fn;                        ///< Emit function
    Acc          init_acc{};                     ///< Initial accumulator value
    size_t       channel_cap = 256;              ///< Input channel capacity
  };

  /**
   * @brief Construct a WindowedAction.
   *
   * @param cfg Configuration.
   */
  explicit WindowedAction(Config cfg)
      : cfg_(std::move(cfg)),
        input_(std::make_shared<AsyncChannel<ContextualItem<T>>>(cfg_.channel_cap)) {}

  WindowedAction(const WindowedAction&) = delete;
  WindowedAction& operator=(const WindowedAction&) = delete;
  WindowedAction(WindowedAction&&)                 = default;
  WindowedAction& operator=(WindowedAction&&)      = default;

  /**
   * @brief Attempt to push an item into the input channel without blocking.
   *
   * If the Context has no EventTime slot, the current wall-clock time is used.
   *
   * @param value Input item.
   * @param ctx   Item context (may contain an EventTime slot).
   * @returns true on success, false if the channel is full or closed.
   */
  bool try_push(T value, Context ctx = {}) {
    return input_->try_send(ContextualItem<T>{std::move(value), std::move(ctx)});
  }

  /**
   * @brief Push an item into the input channel (with backpressure).
   *
   * @param value Input item.
   * @param ctx   Item context.
   * @returns `Result<void>::ok()` or an error.
   */
  Task<Result<void>> push(T value, Context ctx = {}) {
    co_return co_await input_->send(
        ContextualItem<T>{std::move(value), std::move(ctx)});
  }

  /**
   * @brief Start the WindowedAction.
   *
   * Registers the input worker coroutine and ticker coroutine with the Dispatcher.
   *
   * @param dispatcher Dispatcher that will run the coroutines.
   * @param out        Output channel to emit results into.
   */
  void start(Dispatcher& dispatcher,
             std::shared_ptr<AsyncChannel<ContextualItem<Out>>> out = nullptr) {
    out_ = out;
    dispatcher.spawn(input_worker());
    dispatcher.spawn(ticker(tick_interval()));
  }

  /**
   * @brief Manually advance the watermark.
   *
   * Used to inject a watermark from an external source.
   * Applied only if the new watermark is ahead of the current value.
   *
   * @param wm New watermark.
   */
  void advance_watermark(Watermark wm) {
    std::lock_guard lock(state_mutex_);
    if (wm.ts > watermark_) {
      watermark_ = wm.ts;
    }
  }

  /**
   * @brief Close the input channel and wait for all workers to finish.
   */
  Task<void> drain() {
    input_->close();
    while (input_worker_running_.load(std::memory_order_acquire) ||
           ticker_running_.load(std::memory_order_acquire)) {
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
    if (out_)
      out_->close();
    co_return;
  }

  /**
   * @brief Stop immediately.
   */
  void stop() {
    if (stop_src_) stop_src_->request_stop();
    input_->close();
  }

  /**
   * @brief Return the output channel.
   */
  [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<Out>>> output() const {
    return out_;
  }

  /**
   * @brief Return the input channel.
   */
  [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<T>>> input() const {
    return input_;
  }

private:
  // --------------------------------------------------------------------------
  // Internal window state
  // --------------------------------------------------------------------------

  /**
   * @brief State of a single window instance.
   *
   * Manages per-key accumulation state within the window [start, end).
   */
  struct WindowState {
    WindowDesc                       desc;       ///< Window time interval
    std::unordered_map<Key, Acc>     accs;       ///< Per-key accumulators
    system_clock::time_point         last_event; ///< Last event time for session renewal
  };

  // --------------------------------------------------------------------------
  // Tick interval computation
  // --------------------------------------------------------------------------

  /**
   * @brief Return the tick interval based on configuration.
   */
  [[nodiscard]] milliseconds tick_interval() const {
    switch (cfg_.type) {
      case WindowType::Tumbling:
        return cfg_.size;
      case WindowType::Sliding:
        return cfg_.step;
      case WindowType::Session: {
        auto half = cfg_.gap / 2;
        return half.count() > 0 ? half : milliseconds{1};
      }
    }
    return cfg_.size;
  }

  // --------------------------------------------------------------------------
  // 이벤트 시각 추출
  // --------------------------------------------------------------------------

  /**
   * @brief 컨텍스트에서 이벤트 시각을 추출합니다.
   *
   * EventTime 슬롯이 없으면 현재 system_clock 시각을 반환합니다.
   *
   * @param ctx 아이템 컨텍스트.
   * @returns 이벤트 발생 시각.
   */
  [[nodiscard]] static system_clock::time_point extract_event_time(const Context& ctx) {
    if (const auto* et = ctx.get_ptr<EventTime>())
      return et->at;
    return system_clock::now();
  }

  // --------------------------------------------------------------------------
  // 윈도우 상태 갱신
  // --------------------------------------------------------------------------

  /**
   * @brief 아이템을 해당하는 윈도우 상태에 집계합니다.
   *
   * @param item       입력 아이템.
   * @param event_time 이벤트 발생 시각.
   */
  void accumulate(const T& item, system_clock::time_point event_time) {
    std::lock_guard lock(state_mutex_);
    Key key = cfg_.key_fn(item);

    switch (cfg_.type) {
      case WindowType::Tumbling: {
        TumblingWindow tw{cfg_.size};
        auto desc = tw.window_for(event_time);
        auto& ws  = get_or_create_window(desc);
        auto  it  = ws.accs.find(key);
        if (it == ws.accs.end())
          it = ws.accs.emplace(key, cfg_.init_acc).first;
        cfg_.acc_fn(it->second, item);
        break;
      }
      case WindowType::Sliding: {
        SlidingWindow sw{cfg_.size, cfg_.step};
        auto descs = sw.windows_for(event_time);
        for (auto& desc : descs) {
          auto& ws = get_or_create_window(desc);
          auto  it = ws.accs.find(key);
          if (it == ws.accs.end())
            it = ws.accs.emplace(key, cfg_.init_acc).first;
          cfg_.acc_fn(it->second, item);
        }
        break;
      }
      case WindowType::Session: {
        // 세션 윈도우: 키별로 마지막 이벤트 이후 gap 내에 있으면 연장
        // 세션 윈도우를 키별로 찾고 갱신하거나 새로 생성
        bool found = false;
        for (auto& ws : windows_) {
          auto it = ws.accs.find(key);
          if (it != ws.accs.end()) {
            // 이미 이 키에 대한 세션이 있는 경우
            if (event_time <= ws.last_event + cfg_.gap) {
              // 세션 연장
              cfg_.acc_fn(it->second, item);
              // 세션 end를 이벤트+gap으로 확장
              if (event_time + cfg_.gap > ws.desc.end)
                ws.desc.end = event_time + cfg_.gap;
              if (event_time > ws.last_event)
                ws.last_event = event_time;
              found = true;
              break;
            }
          }
        }
        if (!found) {
          // 새 세션 시작
          WindowDesc desc{event_time, event_time + cfg_.gap};
          WindowState ws;
          ws.desc       = desc;
          ws.last_event = event_time;
          ws.accs.emplace(key, cfg_.init_acc);
          cfg_.acc_fn(ws.accs[key], item);
          windows_.push_back(std::move(ws));
        }
        break;
      }
    }

    // 워터마크를 이벤트 시각으로 진전 (단순 진행 모드: 최신 이벤트 기준)
    if (event_time > watermark_)
      watermark_ = event_time;
  }

  /**
   * @brief 해당 WindowDesc에 대한 WindowState를 찾거나 생성합니다.
   *
   * @note state_mutex_ 를 보유한 상태에서 호출해야 합니다.
   *
   * @param desc 찾을 윈도우 디스크립터.
   * @returns 해당 WindowState 참조.
   */
  WindowState& get_or_create_window(const WindowDesc& desc) {
    for (auto& ws : windows_) {
      if (ws.desc.start == desc.start && ws.desc.end == desc.end)
        return ws;
    }
    WindowState ws;
    ws.desc       = desc;
    ws.last_event = desc.start;
    windows_.push_back(std::move(ws));
    return windows_.back();
  }

  // --------------------------------------------------------------------------
  // 완료 윈도우 방출
  // --------------------------------------------------------------------------

  /**
   * @brief 워터마크가 지나간 윈도우를 수집합니다.
   *
   * @returns 방출할 (Key, Acc, window_start) 튜플 목록.
   */
  std::vector<std::tuple<Key, Acc, system_clock::time_point>> collect_completed() {
    std::vector<std::tuple<Key, Acc, system_clock::time_point>> results;
    std::lock_guard lock(state_mutex_);

    auto wm = watermark_;
    std::vector<WindowState> remaining;

    for (auto& ws : windows_) {
      if (ws.desc.end <= wm) {
        // 윈도우 완료 — 모든 키 방출
        for (auto& [k, acc] : ws.accs) {
          results.emplace_back(k, std::move(acc), ws.desc.start);
        }
        // remaining에 추가하지 않음 (제거)
      } else if (cfg_.type == WindowType::Session) {
        // 세션: last_event + gap <= wm 이면 세션 종료
        if (ws.last_event + cfg_.gap <= wm) {
          for (auto& [k, acc] : ws.accs) {
            results.emplace_back(k, std::move(acc), ws.desc.start);
          }
        } else {
          remaining.push_back(std::move(ws));
        }
      } else {
        remaining.push_back(std::move(ws));
      }
    }

    windows_ = std::move(remaining);
    return results;
  }

  // --------------------------------------------------------------------------
  // 입력 워커 코루틴
  // --------------------------------------------------------------------------

  /**
   * @brief 입력 채널에서 아이템을 수신하고 윈도우 상태에 집계합니다.
   *
   * 채널이 닫힐 때까지 반복합니다. EOS 시 남은 윈도우를 강제 방출합니다.
   */
  Task<void> input_worker() {
    input_worker_running_.store(true, std::memory_order_release);

    for (;;) {
      auto citem = co_await input_->recv();
      if (!citem) break; // EOS

      auto event_time = extract_event_time(citem->ctx);
      accumulate(citem->value, event_time);
    }

    // EOS: 남은 모든 윈도우 강제 방출
    {
      std::lock_guard lock(state_mutex_);
      watermark_ = system_clock::time_point::max();
    }

    if (out_) {
      auto completed = collect_completed();
      for (auto& [k, acc, ws] : completed) {
        Out result = cfg_.emit_fn(std::move(k), std::move(acc), ws);
        co_await out_->send(ContextualItem<Out>{std::move(result), Context{}});
      }
    }

    input_worker_running_.store(false, std::memory_order_release);
    co_return;
  }

  // --------------------------------------------------------------------------
  // 틱커 코루틴
  // --------------------------------------------------------------------------

  /**
   * @brief 주기적으로 완료 윈도우를 검사하고 결과를 방출하는 틱커.
   *
   * `interval` 마다 워터마크를 현재 시각으로 진전시키고,
   * 완료된 윈도우를 출력 채널로 방출합니다.
   *
   * 입력 워커가 종료된 후 틱커도 종료됩니다.
   *
   * @param interval 틱 간격.
   */
  Task<void> ticker(milliseconds interval) {
    ticker_running_.store(true, std::memory_order_release);
    stop_src_ = std::make_unique<std::stop_source>();
    auto stop_token = stop_src_->get_token();

    while (!stop_token.stop_requested()) {
      // interval 동안 yield (sleep-based tick)
      auto deadline = std::chrono::steady_clock::now() +
                      std::chrono::duration_cast<std::chrono::nanoseconds>(interval);

      while (std::chrono::steady_clock::now() < deadline) {
        if (stop_token.stop_requested()) goto done;
        if (input_->is_closed() && !input_worker_running_.load(std::memory_order_acquire))
          goto done;

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

      if (out_) {
        auto completed = collect_completed();
        for (auto& [k, acc, ws] : completed) {
          Out result = cfg_.emit_fn(std::move(k), std::move(acc), ws);
          co_await out_->send(ContextualItem<Out>{std::move(result), Context{}});
        }
      }
    }

done:
    ticker_running_.store(false, std::memory_order_release);
    co_return;
  }

  // --------------------------------------------------------------------------
  // 데이터 멤버
  // --------------------------------------------------------------------------

  Config                                                cfg_;
  std::shared_ptr<AsyncChannel<ContextualItem<T>>>      input_;
  std::shared_ptr<AsyncChannel<ContextualItem<Out>>>    out_;
  std::unique_ptr<std::stop_source>                     stop_src_;

  std::atomic<bool> input_worker_running_{false}; ///< 입력 워커 실행 중 여부
  std::atomic<bool> ticker_running_{false};        ///< 틱커 실행 중 여부

  /// @brief 윈도우 상태 및 워터마크 보호 뮤텍스
  std::mutex state_mutex_;
  /// @brief 활성 윈도우 목록
  std::vector<WindowState> windows_;
  /// @brief 현재 워터마크 시각
  system_clock::time_point watermark_{system_clock::time_point::min()};
};

} // namespace qbuem

/** @} */
