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
  // Event time extraction
  // --------------------------------------------------------------------------

  /**
   * @brief Extract the event time from the context.
   *
   * If no EventTime slot is present, returns the current system_clock time.
   *
   * @param ctx Item context.
   * @returns Event occurrence time.
   */
  [[nodiscard]] static system_clock::time_point extract_event_time(const Context& ctx) {
    if (const auto* et = ctx.get_ptr<EventTime>())
      return et->at;
    return system_clock::now();
  }

  // --------------------------------------------------------------------------
  // Window state update
  // --------------------------------------------------------------------------

  /**
   * @brief Accumulate an item into the corresponding window state.
   *
   * @param item       Input item.
   * @param event_time Event occurrence time.
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
        // Session window: if within gap of the last event for this key, extend the session.
        // Find the session window for this key and update it, or create a new one.
        bool found = false;
        for (auto& ws : windows_) {
          auto it = ws.accs.find(key);
          if (it != ws.accs.end()) {
            // A session already exists for this key
            if (event_time <= ws.last_event + cfg_.gap) {
              // Extend the session
              cfg_.acc_fn(it->second, item);
              // Extend session end to event + gap
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
          // Start a new session
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

    // Advance the watermark to the event time (simple mode: based on the latest event)
    if (event_time > watermark_)
      watermark_ = event_time;
  }

  /**
   * @brief Find or create the WindowState for the given WindowDesc.
   *
   * @note Must be called while holding state_mutex_.
   *
   * @param desc Window descriptor to look up.
   * @returns Reference to the corresponding WindowState.
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
  // Completed window emission
  // --------------------------------------------------------------------------

  /**
   * @brief Collect windows whose end time has been passed by the watermark.
   *
   * @returns List of (Key, Acc, window_start) tuples to emit.
   */
  std::vector<std::tuple<Key, Acc, system_clock::time_point>> collect_completed() {
    std::vector<std::tuple<Key, Acc, system_clock::time_point>> results;
    std::lock_guard lock(state_mutex_);

    auto wm = watermark_;
    std::vector<WindowState> remaining;

    for (auto& ws : windows_) {
      if (ws.desc.end <= wm) {
        // Window complete — emit all keys
        for (auto& [k, acc] : ws.accs) {
          results.emplace_back(k, std::move(acc), ws.desc.start);
        }
        // Do not add to remaining (discard)
      } else if (cfg_.type == WindowType::Session) {
        // Session: close if last_event + gap <= wm
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
  // Input worker coroutine
  // --------------------------------------------------------------------------

  /**
   * @brief Receive items from the input channel and accumulate them into window state.
   *
   * Loops until the channel is closed. On EOS, forcibly emits all remaining windows.
   */
  Task<void> input_worker() {
    input_worker_running_.store(true, std::memory_order_release);

    for (;;) {
      auto citem = co_await input_->recv();
      if (!citem) break; // EOS

      auto event_time = extract_event_time(citem->ctx);
      accumulate(citem->value, event_time);
    }

    // EOS: forcibly emit all remaining windows
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
  // Ticker coroutine
  // --------------------------------------------------------------------------

  /**
   * @brief Periodically checks for completed windows and emits results.
   *
   * Every `interval`, advances the watermark to the current time and
   * emits any completed windows to the output channel.
   *
   * The ticker exits after the input worker has terminated.
   *
   * @param interval Tick interval.
   */
  Task<void> ticker(milliseconds interval) {
    ticker_running_.store(true, std::memory_order_release);
    stop_src_ = std::make_unique<std::stop_source>();
    auto stop_token = stop_src_->get_token();

    while (!stop_token.stop_requested()) {
      // Yield for the duration of interval (sleep-based tick)
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
  // Data members
  // --------------------------------------------------------------------------

  Config                                                cfg_;
  std::shared_ptr<AsyncChannel<ContextualItem<T>>>      input_;
  std::shared_ptr<AsyncChannel<ContextualItem<Out>>>    out_;
  std::unique_ptr<std::stop_source>                     stop_src_;

  std::atomic<bool> input_worker_running_{false}; ///< Whether the input worker is running
  std::atomic<bool> ticker_running_{false};        ///< Whether the ticker is running

  /// @brief Mutex protecting window state and watermark
  std::mutex state_mutex_;
  /// @brief List of active windows
  std::vector<WindowState> windows_;
  /// @brief Current watermark timestamp
  system_clock::time_point watermark_{system_clock::time_point::min()};
};

} // namespace qbuem

/** @} */
