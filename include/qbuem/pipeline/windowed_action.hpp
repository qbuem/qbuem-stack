#pragma once

/**
 * @file qbuem/pipeline/windowed_action.hpp
 * @brief v0.9.1 윈도잉 및 이벤트 시간 처리 — WindowedAction, TumblingWindow, SlidingWindow, SessionWindow
 * @defgroup qbuem_windowed_action WindowedAction
 * @ingroup qbuem_pipeline
 *
 * ## 포함된 컴포넌트
 *
 * ### EventTime
 * Context 슬롯 타입으로, 이벤트 원본 발생 시각(system_clock::time_point)을 저장합니다.
 * context.hpp에 이미 선언 및 정의되어 있으며, 이 헤더에서 재사용합니다.
 *
 * ### Watermark
 * 비순서(out-of-order) 이벤트 처리 진행 상태를 나타내는 값 타입입니다.
 * 워터마크 ts까지의 모든 이벤트가 도착했음을 의미합니다.
 *
 * ### TumblingWindow
 * 고정 크기 비중첩 윈도우입니다. size_ms마다 새 윈도우가 시작됩니다.
 *
 * ### SlidingWindow
 * step_ms마다 슬라이딩하며, 각 윈도우 크기는 size_ms입니다 (step < size).
 *
 * ### SessionWindow
 * 이벤트 간 gap_ms 이상 이벤트가 없으면 세션을 종료합니다.
 *
 * ### WindowedAction<T, Key, Acc, Out>
 * 키 기반 시간 윈도우 집계 액션입니다.
 * 워터마크가 윈도우 경계를 넘을 때 결과를 방출합니다.
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
 * @brief 비순서 이벤트 처리 진행 상태를 나타내는 값 타입.
 *
 * `ts` 이전 시각의 모든 이벤트가 이미 도착했음을 보증합니다.
 * 워터마크가 윈도우 경계를 지나면 해당 윈도우의 결과를 방출합니다.
 */
struct Watermark {
  system_clock::time_point ts; ///< 모든 이벤트가 도착했음이 보증된 기준 시각
};

// ============================================================================
// WindowType
// ============================================================================

/**
 * @brief 윈도우 종류 열거형.
 */
enum class WindowType {
  Tumbling, ///< 고정 크기 비중첩 윈도우
  Sliding,  ///< 슬라이딩 윈도우 (step < size)
  Session,  ///< 세션 기반 윈도우 (갭 타임아웃)
};

// ============================================================================
// 내부 윈도우 디스크립터
// ============================================================================

/**
 * @brief 단일 시간 윈도우를 기술하는 내부 타입.
 *
 * 각 윈도우는 [start, end) 구간을 가집니다.
 */
struct WindowDesc {
  system_clock::time_point start; ///< 윈도우 시작 시각 (포함)
  system_clock::time_point end;   ///< 윈도우 종료 시각 (미포함)
};

// ============================================================================
// TumblingWindow
// ============================================================================

/**
 * @brief 고정 크기 비중첩 윈도우 정책.
 *
 * 이벤트 시각 `t`에 대해 소속 윈도우 시작 시각을 계산합니다.
 * 윈도우는 [floor(t / size) * size, floor(t / size) * size + size) 입니다.
 */
struct TumblingWindow {
  milliseconds size; ///< 윈도우 크기

  /**
   * @brief 이벤트 시각으로부터 소속 윈도우를 계산합니다.
   *
   * @param event_time 이벤트 발생 시각.
   * @returns 소속 윈도우 디스크립터.
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
   * @brief 틱 간격을 반환합니다 (tumbling = size).
   */
  [[nodiscard]] milliseconds tick_interval() const { return size; }
};

// ============================================================================
// SlidingWindow
// ============================================================================

/**
 * @brief 슬라이딩 윈도우 정책.
 *
 * 이벤트 시각 `t`가 속할 수 있는 모든 윈도우를 열거합니다.
 * 각 윈도우는 step 간격으로 시작하며, 크기는 size입니다.
 *
 * 이벤트 `t`는 start가 (t - size, t] 범위의 모든 윈도우에 속합니다.
 */
struct SlidingWindow {
  milliseconds size; ///< 윈도우 크기
  milliseconds step; ///< 슬라이딩 간격 (step <= size)

  /**
   * @brief 이벤트 시각이 속하는 모든 윈도우를 반환합니다.
   *
   * @param event_time 이벤트 발생 시각.
   * @returns 이벤트가 속하는 모든 윈도우 목록.
   */
  [[nodiscard]] std::vector<WindowDesc> windows_for(
      system_clock::time_point event_time) const {
    auto epoch_ms  = std::chrono::duration_cast<milliseconds>(
        event_time.time_since_epoch()).count();
    auto size_ms   = size.count();
    auto step_ms   = step.count();

    // 이 이벤트를 포함할 수 있는 가장 이른 윈도우 시작
    // start <= event_time < start + size  =>  event_time - size < start <= event_time
    // start는 step의 배수로 정렬
    auto first_bucket = ((epoch_ms - size_ms) / step_ms + 1) * step_ms;

    std::vector<WindowDesc> result;
    for (auto bucket_ms = first_bucket; bucket_ms <= epoch_ms; bucket_ms += step_ms) {
      auto start = system_clock::time_point(milliseconds(bucket_ms));
      result.push_back(WindowDesc{start, start + size});
    }
    return result;
  }

  /**
   * @brief 틱 간격을 반환합니다 (sliding = step).
   */
  [[nodiscard]] milliseconds tick_interval() const { return step; }
};

// ============================================================================
// SessionWindow
// ============================================================================

/**
 * @brief 세션 기반 윈도우 정책.
 *
 * 이벤트 간 gap_ms 이상 이벤트가 없으면 세션이 종료됩니다.
 * 세션 윈도우는 키 단위로 동적으로 확장됩니다.
 */
struct SessionWindow {
  milliseconds gap; ///< 세션 갭 타임아웃

  /**
   * @brief 틱 간격을 반환합니다 (session = gap / 2, 최소 1ms).
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
 * @brief 키 기반 시간 윈도우 집계 액션.
 *
 * 이벤트 시각(EventTime 컨텍스트 슬롯)과 워터마크를 기반으로 윈도우를 관리합니다.
 * 워터마크가 윈도우 경계를 넘을 때 해당 윈도우의 집계 결과를 방출합니다.
 *
 * ### 윈도우 타입별 동작
 * - **Tumbling**: size_ms마다 새 윈도우, 동시에 하나의 윈도우만 활성
 * - **Sliding**: step_ms마다 슬라이딩, 여러 윈도우가 동시에 활성
 * - **Session**: 이벤트 간 gap_ms 초과 시 세션 종료, 키별 독립 세션 관리
 *
 * ### 워커 코루틴 구조
 * 1. **input_worker**: 입력 채널에서 아이템을 수신하고 윈도우 상태를 갱신합니다.
 * 2. **ticker**: step_ms(또는 gap/2) 간격으로 워터마크를 진전시키고 완료 윈도우를 방출합니다.
 *
 * @tparam T   입력 아이템 타입.
 * @tparam Key 파티셔닝 키 타입 (unordered_map 사용 — std::hash<Key> 필요).
 * @tparam Acc 집계 상태 타입.
 * @tparam Out 방출 결과 타입.
 */
template <typename T, typename Key, typename Acc, typename Out>
class WindowedAction {
public:
  /** @brief 키 추출 함수 타입: `T → Key`. */
  using KeyFn  = std::function<Key(const T&)>;
  /** @brief 집계 함수 타입: `(Acc&, T) → void` (아이템을 누적기에 폴드). */
  using AccFn  = std::function<void(Acc&, const T&)>;
  /** @brief 방출 함수 타입: `(Key, Acc, window_start) → Out`. */
  using EmitFn = std::function<Out(Key, Acc, system_clock::time_point)>;

  /**
   * @brief WindowedAction 설정 구조체.
   */
  struct Config {
    WindowType   type    = WindowType::Tumbling; ///< 윈도우 종류
    milliseconds size    = milliseconds{1000};   ///< 윈도우 크기 (Tumbling/Sliding)
    milliseconds step    = milliseconds{500};    ///< 슬라이딩 간격 (Sliding 전용)
    milliseconds gap     = milliseconds{5000};   ///< 세션 갭 타임아웃 (Session 전용)
    KeyFn        key_fn;                         ///< 키 추출 함수
    AccFn        acc_fn;                         ///< 집계 함수
    EmitFn       emit_fn;                        ///< 방출 함수
    Acc          init_acc{};                     ///< 초기 누적기 값
    size_t       channel_cap = 256;              ///< 입력 채널 용량
  };

  /**
   * @brief WindowedAction을 생성합니다.
   *
   * @param cfg 설정.
   */
  explicit WindowedAction(Config cfg)
      : cfg_(std::move(cfg)),
        input_(std::make_shared<AsyncChannel<ContextualItem<T>>>(cfg_.channel_cap)) {}

  WindowedAction(const WindowedAction&) = delete;
  WindowedAction& operator=(const WindowedAction&) = delete;
  WindowedAction(WindowedAction&&)                 = default;
  WindowedAction& operator=(WindowedAction&&)      = default;

  /**
   * @brief 아이템을 논블로킹으로 입력 채널에 넣으려 시도합니다.
   *
   * Context에 EventTime 슬롯이 없으면 현재 시각을 사용합니다.
   *
   * @param value 입력 아이템.
   * @param ctx   아이템 컨텍스트 (EventTime 슬롯 포함 가능).
   * @returns 성공이면 true, 채널이 가득 찼거나 닫혔으면 false.
   */
  bool try_push(T value, Context ctx = {}) {
    return input_->try_send(ContextualItem<T>{std::move(value), std::move(ctx)});
  }

  /**
   * @brief 아이템을 입력 채널에 넣습니다 (backpressure).
   *
   * @param value 입력 아이템.
   * @param ctx   아이템 컨텍스트.
   * @returns `Result<void>::ok()` 또는 에러.
   */
  Task<Result<void>> push(T value, Context ctx = {}) {
    co_return co_await input_->send(
        ContextualItem<T>{std::move(value), std::move(ctx)});
  }

  /**
   * @brief WindowedAction을 시작합니다.
   *
   * 입력 워커 코루틴과 틱커 코루틴을 Dispatcher에 등록합니다.
   *
   * @param dispatcher 코루틴을 실행할 Dispatcher.
   * @param out        결과를 방출할 출력 채널.
   */
  void start(Dispatcher& dispatcher,
             std::shared_ptr<AsyncChannel<ContextualItem<Out>>> out = nullptr) {
    out_ = out;
    dispatcher.spawn(input_worker());
    dispatcher.spawn(ticker(tick_interval()));
  }

  /**
   * @brief 워터마크를 수동으로 전진시킵니다.
   *
   * 외부에서 워터마크를 주입할 때 사용합니다.
   * 워터마크가 현재 값보다 앞서는 경우에만 적용됩니다.
   *
   * @param wm 새 워터마크.
   */
  void advance_watermark(Watermark wm) {
    std::lock_guard lock(state_mutex_);
    if (wm.ts > watermark_) {
      watermark_ = wm.ts;
    }
  }

  /**
   * @brief 입력 채널을 닫고 워커가 완료될 때까지 기다립니다.
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
   * @brief 즉시 정지합니다.
   */
  void stop() {
    if (stop_src_) stop_src_->request_stop();
    input_->close();
  }

  /**
   * @brief 출력 채널을 반환합니다.
   */
  [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<Out>>> output() const {
    return out_;
  }

  /**
   * @brief 입력 채널을 반환합니다.
   */
  [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<T>>> input() const {
    return input_;
  }

private:
  // --------------------------------------------------------------------------
  // 내부 윈도우 상태
  // --------------------------------------------------------------------------

  /**
   * @brief 단일 윈도우 인스턴스의 상태.
   *
   * 윈도우 [start, end) 내에서 키별 누적 상태를 관리합니다.
   */
  struct WindowState {
    WindowDesc                       desc;       ///< 윈도우 시간 구간
    std::unordered_map<Key, Acc>     accs;       ///< 키별 누적기
    system_clock::time_point         last_event; ///< 세션 갱신용 마지막 이벤트 시각
  };

  // --------------------------------------------------------------------------
  // 틱 간격 계산
  // --------------------------------------------------------------------------

  /**
   * @brief 설정에 따라 틱 간격을 반환합니다.
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
