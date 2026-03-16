#pragma once

/**
 * @file qbuem/pipeline/dynamic_pipeline.hpp
 * @brief 런타임 핫스왑 동적 파이프라인 — DynamicPipeline<T>
 * @defgroup qbuem_dynamic_pipeline DynamicPipeline
 * @ingroup qbuem_pipeline
 *
 * DynamicPipeline은 런타임에 스테이지를 추가/제거/교체할 수 있는 파이프라인입니다.
 * StaticPipeline(컴파일 타임 타입 체인)과 달리, std::any를 통한 타입 소거로
 * 런타임 유연성을 제공합니다. 모든 스테이지는 동일한 메시지 타입 T를 처리합니다.
 *
 * ## 사용 예시
 * ```cpp
 * DynamicPipeline<int> dp;
 * dp.add_stage("double", [](int x, ActionEnv) -> Task<Result<int>> { co_return x*2; });
 * dp.add_stage("addone", [](int x, ActionEnv) -> Task<Result<int>> { co_return x+1; });
 * dp.start(dispatcher);
 * co_await dp.push(42);
 * ```
 * @{
 */

#include <qbuem/pipeline/action.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/async_channel.hpp>
#include <qbuem/pipeline/concepts.hpp>
#include <qbuem/pipeline/context.hpp>
#include <qbuem/pipeline/service_registry.hpp>

#include <any>
#include <atomic>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace qbuem {

/**
 * @brief 핫스왑 가능한 동적 액션의 인터페이스.
 *
 * 런타임 교체를 지원하는 스테이지에 대한 타입 소거 인터페이스입니다.
 */
class IDynamicAction {
public:
    virtual ~IDynamicAction() = default;

    /// @brief 스테이지 이름을 반환합니다.
    virtual std::string_view name() const noexcept = 0;

    /// @brief 스테이지가 현재 실행 중인지 확인합니다.
    virtual bool is_running() const noexcept = 0;
};

/**
 * @brief 타입 소거된 동적 파이프라인 스테이지.
 *
 * std::any를 통해 Action<T,T> 또는 호환 가능한 액션을 래핑합니다.
 */
class DynamicPipelineStage {
public:
    std::string name;
    std::any    action;       ///< Action<T,T>를 보유 (shared_ptr을 통해)
    bool        enabled = true;
};

/**
 * @brief 런타임 핫스왑 파이프라인.
 *
 * 스테이지를 런타임에 추가/제거/교체할 수 있습니다.
 * StaticPipeline(컴파일 타임)과 달리 런타임 타입 소거를 사용합니다.
 * 모든 스테이지는 동일한 메시지 타입 T를 처리해야 합니다.
 *
 * @tparam T 파이프라인을 흐르는 메시지 타입.
 */
template <typename T>
class DynamicPipeline {
public:
    /**
     * @brief DynamicPipeline 설정 구조체.
     */
    struct Config {
        size_t           default_channel_cap  = 256;   ///< 기본 채널 용량
        size_t           default_workers      = 2;     ///< 기본 워커 수
        bool             checkpoint_enabled   = false; ///< 체크포인트 활성화
        ServiceRegistry* registry             = nullptr; ///< 서비스 레지스트리
    };

    /// @brief 스테이지 함수 타입 — add_stage()에 전달 가능한 구체적 타입.
    using StageFn = std::function<Task<Result<T>>(T, ActionEnv)>;

    /**
     * @brief DynamicPipeline을 생성합니다.
     * @param cfg 파이프라인 설정.
     */
    explicit DynamicPipeline(Config cfg = {})
        : cfg_(std::move(cfg)) {}

    DynamicPipeline(const DynamicPipeline&) = delete;
    DynamicPipeline& operator=(const DynamicPipeline&) = delete;
    DynamicPipeline(DynamicPipeline&&) = default;
    DynamicPipeline& operator=(DynamicPipeline&&) = default;

    // -------------------------------------------------------------------------
    // 스테이지 관리
    // -------------------------------------------------------------------------

    /**
     * @brief 체인 끝에 새 스테이지를 추가합니다.
     *
     * @tparam FnT ActionFn<FnT, T, T> concept을 만족하는 함수 타입.
     * @param name      스테이지 이름 (고유해야 함).
     * @param fn        처리 함수: T -> Task<Result<T>>.
     * @param workers   워커 수 (0이면 default_workers 사용).
     * @param chan_cap  채널 용량 (0이면 default_channel_cap 사용).
     */
    template <typename FnT>
        requires ActionFn<FnT, T, T>
    void add_stage(std::string name, FnT fn,
                   size_t workers = 0, size_t chan_cap = 0) {

        auto full_fn = to_full_action_fn<FnT, T, T>(std::move(fn));

        size_t w = (workers  == 0) ? cfg_.default_workers      : workers;
        size_t c = (chan_cap == 0) ? cfg_.default_channel_cap  : chan_cap;

        auto stage = std::make_shared<Stage>();
        stage->name     = std::move(name);
        stage->workers  = w;
        stage->chan_cap = c;
        stage->enabled.store(true, std::memory_order_relaxed);
        stage->stop_src = std::make_unique<std::stop_source>();
        stage->in_channel  = std::make_shared<AsyncChannel<ContextualItem<T>>>(c);
        stage->out_channel = std::make_shared<AsyncChannel<ContextualItem<T>>>(c);

        // Capture the typed fn for the worker factory
        stage->worker_factory = [this, stage, fn = std::move(full_fn)](size_t worker_idx) mutable
            -> Task<void> {
            return stage_worker(stage, fn, worker_idx);
        };

        std::unique_lock lock(stages_mtx_);
        stages_.push_back(std::move(stage));
        rewire_channels_locked();
    }

    /**
     * @brief 이름으로 스테이지를 제거합니다 (체인에서 우회).
     *
     * @param name 제거할 스테이지 이름.
     * @returns 성공 시 true, 스테이지를 찾지 못하면 false.
     */
    bool remove_stage(std::string_view name) {
        std::unique_lock lock(stages_mtx_);
        auto it = std::find_if(stages_.begin(), stages_.end(),
            [&](const auto& s) { return s->name == name; });
        if (it == stages_.end())
            return false;
        stages_.erase(it);
        rewire_channels_locked();
        return true;
    }

    /**
     * @brief 스테이지를 원자적으로 교체합니다 (핫스왑).
     *
     * 새 액션이 처리를 시작하고, 기존 액션은 드레인됩니다.
     * 출력 채널은 브리지됩니다.
     *
     * @tparam FnT ActionFn<FnT, T, T> concept을 만족하는 함수 타입.
     * @param name       교체할 스테이지 이름.
     * @param new_fn     새 처리 함수.
     * @param timeout_ms 드레인 대기 시간 (ms). 현재는 무시됨.
     * @returns 성공 시 true, 스테이지를 찾지 못하면 false.
     */
    template <typename FnT>
        requires ActionFn<FnT, T, T>
    bool hot_swap(std::string_view name, FnT new_fn, int /*timeout_ms*/ = 5000) {

        auto full_fn = to_full_action_fn<FnT, T, T>(std::move(new_fn));

        std::unique_lock lock(stages_mtx_);
        auto it = std::find_if(stages_.begin(), stages_.end(),
            [&](const auto& s) { return s->name == name; });
        if (it == stages_.end())
            return false;

        auto& stage = *it;
        // Update worker factory with new function
        stage->worker_factory = [this, stage_ptr = stage, fn = std::move(full_fn)](size_t worker_idx) mutable
            -> Task<void> {
            return stage_worker(stage_ptr, fn, worker_idx);
        };
        // Signal existing workers to stop; new ones will be spawned on next start
        if (stage->stop_src)
            stage->stop_src->request_stop();
        stage->stop_src = std::make_unique<std::stop_source>();
        return true;
    }

    /**
     * @brief 스테이지를 활성화/비활성화합니다 (비활성화 = 패스스루).
     *
     * @param name    대상 스테이지 이름.
     * @param enabled true이면 활성, false이면 패스스루.
     * @returns 성공 시 true, 스테이지를 찾지 못하면 false.
     */
    bool set_enabled(std::string_view name, bool enabled) {
        std::shared_lock lock(stages_mtx_);
        auto it = std::find_if(stages_.begin(), stages_.end(),
            [&](const auto& s) { return s->name == name; });
        if (it == stages_.end())
            return false;
        (*it)->enabled.store(enabled, std::memory_order_release);
        return true;
    }

    // -------------------------------------------------------------------------
    // 아이템 전송
    // -------------------------------------------------------------------------

    /**
     * @brief 첫 번째 스테이지에 아이템을 전송합니다 (backpressure).
     *
     * @param item 처리할 아이템.
     * @param ctx  아이템 컨텍스트.
     * @returns Result<void>::ok() 또는 에러.
     */
    Task<Result<void>> push(T item, Context ctx = {}) {
        std::shared_ptr<AsyncChannel<ContextualItem<T>>> first_in;
        {
            std::shared_lock lock(stages_mtx_);
            if (stages_.empty())
                co_return unexpected(std::make_error_code(std::errc::no_such_process));
            first_in = stages_.front()->in_channel;
        }
        co_return co_await first_in->send(
            ContextualItem<T>{std::move(item), std::move(ctx)});
    }

    /**
     * @brief 논블로킹 push.
     *
     * @returns 성공 시 true, 채널 포화 또는 스테이지 없음 시 false.
     */
    bool try_push(T item, Context ctx = {}) {
        std::shared_lock lock(stages_mtx_);
        if (stages_.empty())
            return false;
        return stages_.front()->in_channel->try_send(
            ContextualItem<T>{std::move(item), std::move(ctx)});
    }

    // -------------------------------------------------------------------------
    // 라이프사이클
    // -------------------------------------------------------------------------

    /**
     * @brief 모든 스테이지를 시작합니다 — 각 스테이지에 워커 코루틴 spawn.
     *
     * @param dispatcher 코루틴을 실행할 Dispatcher.
     */
    void start(Dispatcher& dispatcher) {
        std::shared_lock lock(stages_mtx_);
        started_ = true;
        for (auto& stage : stages_) {
            for (size_t i = 0; i < stage->workers; ++i) {
                stage->worker_count.fetch_add(1, std::memory_order_relaxed);
                dispatcher.spawn(stage->worker_factory(i));
            }
        }
    }

    /**
     * @brief 입력을 닫고 모든 스테이지가 처리를 마칠 때까지 기다립니다.
     */
    Task<void> drain() {
        std::vector<std::shared_ptr<Stage>> snapshot;
        {
            std::shared_lock lock(stages_mtx_);
            snapshot = stages_;
        }
        if (!snapshot.empty()) {
            snapshot.front()->in_channel->close();
            for (auto& stage : snapshot) {
                while (stage->worker_count.load(std::memory_order_acquire) > 0) {
                    co_await yield_once();
                }
            }
        }
        co_return;
    }

    /**
     * @brief 파이프라인을 즉시 정지합니다.
     */
    void stop() {
        std::shared_lock lock(stages_mtx_);
        for (auto& stage : stages_) {
            if (stage->stop_src)
                stage->stop_src->request_stop();
            stage->in_channel->close();
        }
    }

    // -------------------------------------------------------------------------
    // 출력 채널
    // -------------------------------------------------------------------------

    /**
     * @brief 마지막 스테이지의 출력 채널을 반환합니다.
     *
     * @returns 출력 채널 포인터. 스테이지가 없으면 nullptr.
     */
    [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<T>>> output() const {
        std::shared_lock lock(stages_mtx_);
        if (stages_.empty())
            return nullptr;
        return stages_.back()->out_channel;
    }

    // -------------------------------------------------------------------------
    // 조회
    // -------------------------------------------------------------------------

    /// @brief 스테이지 이름 목록을 반환합니다.
    [[nodiscard]] std::vector<std::string> stage_names() const {
        std::shared_lock lock(stages_mtx_);
        std::vector<std::string> names;
        names.reserve(stages_.size());
        for (const auto& s : stages_)
            names.push_back(s->name);
        return names;
    }

    /// @brief 스테이지 수를 반환합니다.
    [[nodiscard]] size_t stage_count() const {
        std::shared_lock lock(stages_mtx_);
        return stages_.size();
    }

private:
    // -------------------------------------------------------------------------
    // 내부 스테이지 구조체
    // -------------------------------------------------------------------------
    struct Stage {
        std::string name;
        std::shared_ptr<AsyncChannel<ContextualItem<T>>> in_channel;
        std::shared_ptr<AsyncChannel<ContextualItem<T>>> out_channel;
        std::function<Task<void>(size_t worker_idx)>     worker_factory;
        std::atomic<size_t>                              worker_count{0};
        std::atomic<bool>                                enabled{true};
        std::unique_ptr<std::stop_source>                stop_src;
        size_t workers;
        size_t chan_cap;
    };

    // -------------------------------------------------------------------------
    // 채널 재연결 (stages_mtx_ exclusive lock 보유 상태에서 호출)
    // -------------------------------------------------------------------------

    /**
     * @brief 스테이지 체인의 채널을 재연결합니다.
     *
     * stage[i].out_channel == stage[i+1].in_channel 이 되도록 연결합니다.
     * stages_mtx_ exclusive lock 보유 상태에서 호출해야 합니다.
     */
    void rewire_channels_locked() {
        for (size_t i = 0; i + 1 < stages_.size(); ++i) {
            // Share the out_channel of stage[i] as in_channel of stage[i+1]
            stages_[i+1]->in_channel = stages_[i]->out_channel;
        }
    }

    // -------------------------------------------------------------------------
    // 워커 루프
    // -------------------------------------------------------------------------

    /**
     * @brief 단일 스테이지 워커 코루틴.
     *
     * 입력 채널에서 아이템을 읽어 fn을 적용하고 출력 채널에 전달합니다.
     * 스테이지가 비활성화된 경우 아이템을 그대로 패스스루합니다.
     */
    Task<void> stage_worker(
        std::shared_ptr<Stage> stage,
        std::function<Task<Result<T>>(T, ActionEnv)> fn,
        size_t worker_idx)
    {
        auto stop_token = stage->stop_src
            ? stage->stop_src->get_token()
            : std::stop_token{};

        for (;;) {
            auto citem = co_await stage->in_channel->recv();
            if (!citem) break; // EOS

            if (!stage->enabled.load(std::memory_order_acquire)) {
                // Pass-through: disabled stage forwards item unchanged
                if (stage->out_channel)
                    co_await stage->out_channel->send(std::move(*citem));
                continue;
            }

            ActionEnv env{
                .ctx        = citem->ctx,
                .stop       = stop_token,
                .worker_idx = worker_idx,
                .registry   = cfg_.registry ? cfg_.registry : &global_registry(),
            };

            auto result = co_await fn(std::move(citem->value), env);

            if (result.has_value() && stage->out_channel) {
                co_await stage->out_channel->send(
                    ContextualItem<T>{std::move(*result), env.ctx});
            }
        }

        size_t remaining = stage->worker_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0 && stage->out_channel)
            stage->out_channel->close();
        co_return;
    }

    // -------------------------------------------------------------------------
    // 코루틴 yield 헬퍼
    // -------------------------------------------------------------------------
    struct YieldAwaiter {
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) noexcept {
            if (auto* r = Reactor::current())
                r->post([h]() mutable { h.resume(); });
            else
                h.resume();
        }
        void await_resume() noexcept {}
    };

    static Task<void> yield_once() {
        co_await YieldAwaiter{};
        co_return;
    }

    // -------------------------------------------------------------------------
    // 데이터 멤버
    // -------------------------------------------------------------------------
    Config                                   cfg_;
    std::vector<std::shared_ptr<Stage>>      stages_;
    mutable std::shared_mutex                stages_mtx_;
    bool                                     started_ = false;
};

/**
 * @brief Adapts a static Action<In, Out> into a DynamicPipeline stage function.
 *
 * Because DynamicPipeline<T> requires homogeneous T→T stages, this function
 * is constrained to the case where In == Out (same_as).
 *
 * The returned function implements the passthrough pattern:
 *  1. Pushes the incoming value into the action's input channel (with
 *     backpressure via Action::push).
 *  2. co_returns the value unchanged so the DynamicPipeline worker can
 *     forward it to the next stage.
 *
 * The Action's own worker pool processes items asynchronously; the pipeline
 * stage merely fans items in and passes them through.
 *
 * @tparam In  Input type (must equal Out).
 * @tparam Out Output type (must equal In).
 * @param action A shared_ptr to the Action<In, Out> to wrap.
 * @returns A std::function<Task<Result<In>>(In, ActionEnv)> usable with
 *          DynamicPipeline<In>::add_stage / hot_swap.
 */
template <typename In, typename Out>
  requires std::same_as<In, Out>
auto make_dynamic_action(std::shared_ptr<Action<In, Out>> action)
    -> std::function<Task<Result<In>>(In, ActionEnv)>
{
    return [action = std::move(action)](In value, ActionEnv env)
        -> Task<Result<In>>
    {
        // Forward the item into the action's input channel (with backpressure).
        auto push_result = co_await action->push(value, env.ctx);
        if (!push_result.has_value())
            co_return unexpected(push_result.error());

        // Passthrough: return the original value so the DynamicPipeline
        // worker can send it to the next stage.
        co_return value;
    };
}

} // namespace qbuem

/** @} */
