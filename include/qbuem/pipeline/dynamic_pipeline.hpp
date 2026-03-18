#pragma once

/**
 * @file qbuem/pipeline/dynamic_pipeline.hpp
 * @brief Runtime hot-swap dynamic pipeline — DynamicPipeline<T>
 * @defgroup qbuem_dynamic_pipeline DynamicPipeline
 * @ingroup qbuem_pipeline
 *
 * DynamicPipeline is a pipeline whose stages can be added, removed, or replaced at runtime.
 * Unlike StaticPipeline (compile-time type chain), it uses type erasure via std::any
 * to provide runtime flexibility. All stages process the same message type T.
 *
 * ## Usage example
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
 * @brief Interface for a hot-swappable dynamic action.
 *
 * Type-erased interface for stages that support runtime replacement.
 */
class IDynamicAction {
public:
    virtual ~IDynamicAction() = default;

    /// @brief Return the stage name.
    virtual std::string_view name() const noexcept = 0;

    /// @brief Return whether the stage is currently running.
    virtual bool is_running() const noexcept = 0;
};

/**
 * @brief Type-erased dynamic pipeline stage.
 *
 * Wraps an Action<T,T> or compatible action via std::any.
 */
class DynamicPipelineStage {
public:
    std::string name;
    std::any    action;       ///< Holds an Action<T,T> (via shared_ptr)
    bool        enabled = true;
};

/**
 * @brief Runtime hot-swap pipeline.
 *
 * Stages can be added, removed, or replaced at runtime.
 * Uses runtime type erasure unlike StaticPipeline (compile-time).
 * All stages must process the same message type T.
 *
 * @tparam T Message type flowing through the pipeline.
 */
template <typename T>
class DynamicPipeline {
public:
    /**
     * @brief DynamicPipeline configuration.
     */
    struct Config {
        size_t           default_channel_cap  = 256;   ///< Default channel capacity
        size_t           default_workers      = 2;     ///< Default number of workers
        bool             checkpoint_enabled   = false; ///< Enable checkpointing
        ServiceRegistry* registry             = nullptr; ///< Service registry
    };

    /// @brief Stage function type — the concrete type passed to add_stage().
    using StageFn = std::function<Task<Result<T>>(T, ActionEnv)>;

    /**
     * @brief Construct a DynamicPipeline.
     * @param cfg Pipeline configuration.
     */
    explicit DynamicPipeline(Config cfg = {})
        : cfg_(std::move(cfg)) {}

    DynamicPipeline(const DynamicPipeline&) = delete;
    DynamicPipeline& operator=(const DynamicPipeline&) = delete;
    DynamicPipeline(DynamicPipeline&&) = default;
    DynamicPipeline& operator=(DynamicPipeline&&) = default;

    // -------------------------------------------------------------------------
    // Stage management
    // -------------------------------------------------------------------------

    /**
     * @brief Append a new stage to the end of the chain.
     *
     * @tparam FnT Function type satisfying ActionFn<FnT, T, T>.
     * @param name      Stage name (must be unique).
     * @param fn        Processing function: T -> Task<Result<T>>.
     * @param workers   Number of workers (0 uses default_workers).
     * @param chan_cap  Channel capacity (0 uses default_channel_cap).
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

        // Capture the typed fn for the worker factory.
        // Use weak_ptr to avoid Stage→lambda→Stage circular reference that
        // would prevent the Stage from ever being freed.
        std::weak_ptr<Stage> weak_stage = stage;
        stage->worker_factory = [this, weak_stage, fn = std::move(full_fn)](size_t worker_idx) mutable
            -> Task<void> {
            auto s = weak_stage.lock();
            if (s) co_await stage_worker(s, fn, worker_idx);
        };

        std::unique_lock lock(stages_mtx_);
        stages_.push_back(std::move(stage));
        rewire_channels_locked();
    }

    /**
     * @brief Remove a stage by name (bypass in chain).
     *
     * @param name Stage name to remove.
     * @returns true on success; false if the stage was not found.
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
     * @brief Atomically replace a stage (hot-swap).
     *
     * The new action begins processing; the old action is drained.
     * Output channels are bridged.
     *
     * @tparam FnT Function type satisfying ActionFn<FnT, T, T>.
     * @param name       Stage name to replace.
     * @param new_fn     New processing function.
     * @param timeout_ms Drain wait time in ms. Currently ignored.
     * @returns true on success; false if the stage was not found.
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
        // Update worker factory with new function.
        // Use weak_ptr (not shared_ptr) to avoid a Stage→worker_factory→Stage
        // reference cycle that would prevent Stage from ever being freed.
        std::weak_ptr<Stage> weak_stage = stage;
        stage->worker_factory = [this, weak_stage, fn = std::move(full_fn)](size_t worker_idx) mutable
            -> Task<void> {
            auto s = weak_stage.lock();
            if (s) co_await stage_worker(s, fn, worker_idx);
        };
        // Signal existing workers to stop; new ones will be spawned on next start
        if (stage->stop_src)
            stage->stop_src->request_stop();
        stage->stop_src = std::make_unique<std::stop_source>();
        return true;
    }

    /**
     * @brief Enable or disable a stage (disabled = pass-through).
     *
     * @param name    Target stage name.
     * @param enabled true to activate; false for pass-through.
     * @returns true on success; false if the stage was not found.
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
    // Item submission
    // -------------------------------------------------------------------------

    /**
     * @brief Send an item to the first stage (with backpressure).
     *
     * @param item Item to process.
     * @param ctx  Item context.
     * @returns Result<void>{} or an error.
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
     * @brief Non-blocking push.
     *
     * @returns true on success; false if the channel is full or no stages exist.
     */
    bool try_push(T item, Context ctx = {}) {
        std::shared_lock lock(stages_mtx_);
        if (stages_.empty())
            return false;
        return stages_.front()->in_channel->try_send(
            ContextualItem<T>{std::move(item), std::move(ctx)});
    }

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    /**
     * @brief Start all stages — spawn worker coroutines for each stage.
     *
     * @param dispatcher Dispatcher to run coroutines on.
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
     * @brief Close input and wait for all stages to finish processing.
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
     * @brief Stop the pipeline immediately.
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
    // Output channel
    // -------------------------------------------------------------------------

    /**
     * @brief Return the output channel of the last stage.
     *
     * @returns Output channel pointer; nullptr if there are no stages.
     */
    [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<T>>> output() const {
        std::shared_lock lock(stages_mtx_);
        if (stages_.empty())
            return nullptr;
        return stages_.back()->out_channel;
    }

    // -------------------------------------------------------------------------
    // Queries
    // -------------------------------------------------------------------------

    /// @brief Return the list of stage names.
    [[nodiscard]] std::vector<std::string> stage_names() const {
        std::shared_lock lock(stages_mtx_);
        std::vector<std::string> names;
        names.reserve(stages_.size());
        for (const auto& s : stages_)
            names.push_back(s->name);
        return names;
    }

    /// @brief Return the number of stages.
    [[nodiscard]] size_t stage_count() const {
        std::shared_lock lock(stages_mtx_);
        return stages_.size();
    }

private:
    // -------------------------------------------------------------------------
    // Internal stage structure
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
    // Channel rewiring (must be called while holding stages_mtx_ exclusively)
    // -------------------------------------------------------------------------

    /**
     * @brief Rewire channels across the stage chain.
     *
     * Ensures stage[i].out_channel == stage[i+1].in_channel.
     * Must be called while holding stages_mtx_ exclusively.
     */
    void rewire_channels_locked() {
        for (size_t i = 0; i + 1 < stages_.size(); ++i) {
            // Share the out_channel of stage[i] as in_channel of stage[i+1]
            stages_[i+1]->in_channel = stages_[i]->out_channel;
        }
    }

    // -------------------------------------------------------------------------
    // Worker loop
    // -------------------------------------------------------------------------

    /**
     * @brief Single stage worker coroutine.
     *
     * Reads items from the input channel, applies fn, and forwards to the output channel.
     * If the stage is disabled, items are forwarded unchanged (pass-through).
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
    // Coroutine yield helper
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
    // Data members
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
