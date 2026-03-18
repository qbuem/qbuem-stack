#pragma once

/**
 * @file qbuem/pipeline/message_bus.hpp
 * @brief Topic-based publish/subscribe message bus — MessageBus, SubpipelineAction
 * @defgroup qbuem_message_bus MessageBus
 * @ingroup qbuem_pipeline
 *
 * MessageBus supports topic-based pub/sub patterns and streaming.
 * Four patterns are supported:
 * - Unary (1→1): single publisher → single subscriber (round-robin)
 * - ServerStream (1→N): single publish, stream channel receive
 * - ClientStream (N→1): accumulate from channel then produce single result
 * - Bidi: bidirectional streaming
 *
 * ## Usage example
 * ```cpp
 * MessageBus bus;
 * bus.start(dispatcher);
 *
 * auto sub = bus.subscribe("events", [](MessageBus::Msg msg, Context) -> Task<Result<void>> {
 *     auto& ev = std::any_cast<MyEvent&>(msg);
 *     co_return Result<void>{};
 * });
 *
 * co_await bus.publish("events", MyEvent{...});
 * ```
 * @{
 */

#include <qbuem/pipeline/action.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/async_channel.hpp>
#include <qbuem/pipeline/context.hpp>
#include <qbuem/pipeline/service_registry.hpp>
#include <qbuem/pipeline/static_pipeline.hpp>

#include <any>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace qbuem {

/// @brief Transparent hash for std::string_view — prevents temporary
///        `std::string(topic)` construction during publish/subscribe topic lookup.
struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
};

/**
 * @brief Topic-based publish/subscribe message bus.
 *
 * Messages are type-erased via std::any for delivery.
 * The subscription handle (Subscription) destructor automatically cancels the subscription.
 */
class MessageBus {
public:
    /// @brief Type-erased message type.
    using Msg = std::any;

    /// @brief Message handler type.
    using Handler = std::function<Task<Result<void>>(Msg, Context)>;

    // -------------------------------------------------------------------------
    // Subscription handle
    // -------------------------------------------------------------------------

    /**
     * @brief Subscription handle — automatically cancels subscription on destruction.
     *
     * Manages subscription lifetime in RAII fashion.
     * Movable but not copyable.
     */
    class Subscription {
    public:
        Subscription() = default;

        /**
         * @brief Constructs a subscription handle.
         * @param topic Subscription topic.
         * @param id    Subscription ID.
         * @param bus   Owning MessageBus pointer.
         */
        Subscription(std::string topic, size_t id, MessageBus* bus)
            : topic_(std::move(topic))
            , id_(id)
            , bus_(bus)
            , active_(true)
        {}

        ~Subscription() { cancel(); }

        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;

        Subscription(Subscription&& o) noexcept
            : topic_(std::move(o.topic_))
            , id_(o.id_)
            , bus_(o.bus_)
            , active_(o.active_)
        {
            o.active_ = false;
            o.bus_    = nullptr;
        }

        Subscription& operator=(Subscription&& o) noexcept {
            if (this != &o) {
                cancel();
                topic_   = std::move(o.topic_);
                id_      = o.id_;
                bus_     = o.bus_;
                active_  = o.active_;
                o.active_ = false;
                o.bus_    = nullptr;
            }
            return *this;
        }

        /// @brief Explicitly cancels the subscription.
        void cancel() {
            if (active_ && bus_) {
                bus_->unsubscribe(topic_, id_);
                active_ = false;
                bus_    = nullptr;
            }
        }

        /// @brief Returns whether the subscription is active.
        [[nodiscard]] bool is_active() const noexcept { return active_; }

    private:
        std::string  topic_;
        size_t       id_     = 0;
        MessageBus*  bus_    = nullptr;
        bool         active_ = false;
    };

    // -------------------------------------------------------------------------
    // Constructor / Destructor
    // -------------------------------------------------------------------------

    MessageBus() = default;
    ~MessageBus() = default;

    MessageBus(const MessageBus&) = delete;
    MessageBus& operator=(const MessageBus&) = delete;

    // -------------------------------------------------------------------------
    // Subscribe
    // -------------------------------------------------------------------------

    /**
     * @brief Unary pattern subscription — message is delivered directly to the handler.
     *
     * If multiple subscribers exist on the same topic, fan-out (all receive).
     *
     * @param topic    Topic to subscribe to.
     * @param handler  Message handler function.
     * @param chan_cap Internal channel capacity.
     * @returns Subscription handle (automatically cancelled on destruction).
     */
    Subscription subscribe(std::string topic, Handler handler,
                           size_t /*chan_cap*/ = 64) {
        auto state = get_or_create_topic(topic);
        size_t id = state->next_id.fetch_add(1, std::memory_order_relaxed);

        Sub sub;
        sub.id      = id;
        sub.handler = std::move(handler);
        // No internal channel: handlers are dispatched directly via invoke_handler.

        {
            std::unique_lock lock(state->mtx);
            state->subs.push_back(std::move(sub));
        }

        return Subscription(topic, id, this);
    }

    /**
     * @brief Publishes a message to all subscribers of a topic (fan-out, backpressure).
     *
     * @tparam T Message type.
     * @param topic Topic to publish to.
     * @param msg   Message to publish.
     * @param ctx   Message context.
     * @returns Result<void>{} or an error.
     */
    template <typename T>
    Task<Result<void>> publish(std::string_view topic, T msg, Context ctx = {}) {
        std::shared_ptr<TopicState> state;
        {
            std::shared_lock lock(topics_mtx_);
            auto it = topics_.find(topic);
            if (it == topics_.end())
                co_return Result<void>{};
            state = it->second;
        }

        if (state->closed.load(std::memory_order_acquire))
            co_return unexpected(std::make_error_code(std::errc::broken_pipe));

        Msg any_msg = std::move(msg);

        std::vector<Handler> handlers;
        std::vector<std::function<bool(const Msg&)>> direct_senders;
        {
            std::shared_lock lock(state->mtx);
            // Pre-reserve to the number of subs — prevents reallocation on each publish
            handlers.reserve(state->subs.size());
            direct_senders.reserve(state->subs.size());
            for (auto& sub : state->subs) {
                if (sub.direct_sender)
                    direct_senders.push_back(sub.direct_sender);
                else if (sub.handler)
                    handlers.push_back(sub.handler);
            }
        }

        // Direct (subscribe_stream) senders — non-blocking to avoid deadlock
        for (auto& ds : direct_senders)
            ds(any_msg);

        // Directly co_await each handler so publish() provides backpressure and
        // the caller knows handlers have run before publish() returns.
        for (auto& h : handlers)
            co_await invoke_handler(h, any_msg, ctx);
        co_return Result<void>{};
    }

    /**
     * @brief Non-blocking publish (drops if channel is full).
     *
     * @tparam T Message type.
     * @param topic Topic to publish to.
     * @param msg   Message to publish.
     * @param ctx   Message context.
     * @returns true if successfully sent to all subscribers.
     */
    template <typename T>
    bool try_publish(std::string_view topic, T msg, Context /*ctx*/ = {}) {
        std::shared_ptr<TopicState> state;
        {
            std::shared_lock lock(topics_mtx_);
            auto it = topics_.find(topic);
            if (it == topics_.end())
                return true; // no subscribers, silently drop
            state = it->second;
        }

        if (state->closed.load(std::memory_order_acquire))
            return false;

        Msg any_msg = std::move(msg);
        bool all_ok = true;

        std::shared_lock lock(state->mtx);
        for (auto& sub : state->subs) {
            if (sub.direct_sender) {
                if (!sub.direct_sender(any_msg)) all_ok = false;
            } else if (sub.channel) {
                if (!sub.channel->try_send(any_msg)) all_ok = false;
            }
        }
        return all_ok;
    }

    /**
     * @brief ServerStream subscription — receives a stream via a channel.
     *
     * The returned channel streams messages published to the topic.
     * When the topic is closed, the channel is also closed.
     *
     * @tparam T Message type.
     * @param topic Topic to subscribe to.
     * @param cap   Channel capacity.
     * @returns Message stream channel.
     */
    template <typename T>
    std::shared_ptr<AsyncChannel<T>> subscribe_stream(std::string topic,
                                                       size_t cap = 256) {
        auto stream_ch = std::make_shared<AsyncChannel<T>>(cap);

        // Subscribe with a handler that forwards typed messages to stream_ch
        auto state = get_or_create_topic(topic);
        size_t id = state->next_id.fetch_add(1, std::memory_order_relaxed);

        Sub sub;
        sub.id = id;
        // Use direct_sender to forward type-erased Msg into the typed stream_ch.
        // This bypasses the handler indirection (which requires a dispatcher worker)
        // and makes subscribe_stream work without any background workers.
        sub.direct_sender = [typed_ch = stream_ch](const Msg& m) -> bool {
            if (const T* p = std::any_cast<T>(&m)) {
                return typed_ch->try_send(*p);
            }
            return false;
        };

        {
            std::unique_lock lock(state->mtx);
            state->subs.push_back(std::move(sub));
        }

        return stream_ch;
    }

    /**
     * @brief ClientStream subscription — accumulates from a channel and produces a single result.
     *
     * @tparam In  Input message type.
     * @tparam Out Output result type.
     * @param topic  Topic to subscribe to.
     * @param fn     Accumulation function: AsyncChannel<In> -> Task<Result<Out>>.
     * @param cap    Internal channel capacity.
     * @returns Subscription handle.
     */
    template <typename In, typename Out>
    Subscription subscribe_accumulate(
        std::string topic,
        std::function<Task<Result<Out>>(std::shared_ptr<AsyncChannel<In>>)> fn,
        size_t cap = 256)
    {
        auto accumulate_ch = std::make_shared<AsyncChannel<In>>(cap);

        // The handler feeds into the accumulate channel
        Handler handler = [accumulate_ch](Msg m, Context) -> Task<Result<void>> {
            if (const In* p = std::any_cast<In>(&m)) {
                co_await accumulate_ch->send(*p);
            }
            co_return Result<void>{};
        };

        // Spawn the accumulation coroutine via the dispatcher (if started)
        if (dispatcher_) {
            auto run_accumulator =
                [fn2 = std::move(fn), ch = accumulate_ch]() mutable -> Task<void> {
                    co_await fn2(ch);
                };
            dispatcher_->spawn(run_accumulator());
        }

        return subscribe(std::move(topic), std::move(handler), cap);
    }

    /**
     * @brief Closes a topic — propagates EOS to all subscribers.
     *
     * @param topic Name of the topic to close.
     */
    void close_topic(std::string_view topic) {
        std::shared_ptr<TopicState> state;
        {
            std::shared_lock lock(topics_mtx_);
            auto it = topics_.find(topic);
            if (it == topics_.end())
                return;
            state = it->second;
        }
        state->closed.store(true, std::memory_order_release);
        std::shared_lock lock(state->mtx);
        for (auto& sub : state->subs) {
            if (sub.channel)
                sub.channel->close();
        }
    }

    // -------------------------------------------------------------------------
    // Statistics
    // -------------------------------------------------------------------------

    /**
     * @brief Returns the number of subscribers for a given topic.
     */
    [[nodiscard]] size_t subscriber_count(std::string_view topic) const {
        std::shared_lock outer(topics_mtx_);
        auto it = topics_.find(topic);
        if (it == topics_.end())
            return 0;
        std::shared_lock inner(it->second->mtx);
        return it->second->subs.size();
    }

    /**
     * @brief Returns the names of all currently existing topics.
     */
    [[nodiscard]] std::vector<std::string> topics() const {
        std::shared_lock lock(topics_mtx_);
        std::vector<std::string> result;
        result.reserve(topics_.size());
        for (const auto& [name, _] : topics_)
            result.push_back(name);
        return result;
    }

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    /**
     * @brief Starts the MessageBus.
     *
     * @param dispatcher Dispatcher used to run internal workers.
     */
    void start(Dispatcher& dispatcher) {
        dispatcher_ = &dispatcher;
    }

    /**
     * @brief Closes and drains all topics.
     */
    Task<void> drain() {
        std::vector<std::string> topic_names;
        {
            std::shared_lock lock(topics_mtx_);
            for (const auto& [name, _] : topics_)
                topic_names.push_back(name);
        }
        for (const auto& t : topic_names)
            close_topic(t);
        co_return;
    }

private:
    // -------------------------------------------------------------------------
    // Internal structs
    // -------------------------------------------------------------------------

    struct Sub {
        size_t                                   id;
        Handler                                  handler;
        std::shared_ptr<AsyncChannel<Msg>>       channel;
        // For subscribe_stream: bypass the handler indirection and directly
        // try_send the type-erased message into a typed channel.
        // Set instead of channel when subscribe_stream is used.
        std::function<bool(const Msg&)>          direct_sender;
    };

    struct TopicState {
        mutable std::shared_mutex      mtx;
        std::vector<Sub>               subs;
        std::atomic<size_t>            next_id{1};
        std::atomic<bool>              closed{false};
    };

    // -------------------------------------------------------------------------
    // Internal helpers
    // -------------------------------------------------------------------------

    /**
     * @brief Returns the topic state, creating it if it does not exist.
     */
    std::shared_ptr<TopicState> get_or_create_topic(const std::string& topic) {
        {
            std::shared_lock lock(topics_mtx_);
            auto it = topics_.find(topic);
            if (it != topics_.end())
                return it->second;
        }
        std::unique_lock lock(topics_mtx_);
        // Double-checked locking
        auto it = topics_.find(topic);
        if (it != topics_.end())
            return it->second;
        auto state = std::make_shared<TopicState>();
        topics_[topic] = state;
        return state;
    }

    // Named coroutine — prevents HALO from placing the frame on publish()'s stack.
    static Task<void> invoke_handler(Handler h, Msg msg, Context ctx) {
        co_await h(std::move(msg), std::move(ctx));
    }

    /**
     * @brief Removes the subscription with the given ID from the specified topic.
     */
    void unsubscribe(std::string_view topic, size_t id) {
        std::shared_ptr<TopicState> state;
        {
            std::shared_lock lock(topics_mtx_);
            auto it = topics_.find(topic);
            if (it == topics_.end())
                return;
            state = it->second;
        }
        std::unique_lock lock(state->mtx);
        auto& subs = state->subs;
        subs.erase(
            std::remove_if(subs.begin(), subs.end(),
                [id](const Sub& s) { return s.id == id; }),
            subs.end());
    }

    // -------------------------------------------------------------------------
    // Data members
    // -------------------------------------------------------------------------
    mutable std::shared_mutex                                                   topics_mtx_;
    std::unordered_map<std::string, std::shared_ptr<TopicState>,
                       StringHash, std::equal_to<>>                             topics_;
    Dispatcher*                                                  dispatcher_ = nullptr;
};

// =============================================================================
// SubpipelineAction
// =============================================================================

/**
 * @brief Wraps a StaticPipeline<In,Out> as a single Action<In,Out>.
 *
 * Used to embed a StaticPipeline as a stage inside a DynamicPipeline.
 *
 * @tparam In  Input type.
 * @tparam Out Output type.
 */
template <typename In, typename Out>
class SubpipelineAction {
public:
    /**
     * @brief Constructs a SubpipelineAction from a StaticPipeline.
     *
     * @param pipeline A StaticPipeline with stages already added.
     */
    explicit SubpipelineAction(StaticPipeline<In, Out> pipeline)
        : pipeline_(std::move(pipeline))
    {}

    /**
     * @brief Sends an item to the internal pipeline (with backpressure).
     */
    Task<Result<void>> push(In item, Context ctx = {}) {
        return pipeline_.push(std::move(item), std::move(ctx));
    }

    /**
     * @brief Non-blocking push.
     */
    bool try_push(In item, Context ctx = {}) {
        return pipeline_.try_push(std::move(item), std::move(ctx));
    }

    /**
     * @brief Starts the internal pipeline.
     *
     * @param dispatcher Dispatcher used to run workers.
     * @param out        Output channel (if nullptr, the pipeline uses its own internal output).
     */
    void start(Dispatcher& dispatcher,
               std::shared_ptr<AsyncChannel<ContextualItem<Out>>> out = nullptr) {
        (void)out; // StaticPipeline manages its own tail channel
        pipeline_.start(dispatcher);
    }

    /**
     * @brief Drain: closes input and waits until all processing is complete.
     */
    Task<void> drain() {
        return pipeline_.drain();
    }

    /**
     * @brief Stops immediately.
     */
    void stop() {
        pipeline_.stop();
    }

    /**
     * @brief Returns the output channel of the internal pipeline.
     */
    [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<Out>>> output() const {
        return pipeline_.output();
    }

private:
    StaticPipeline<In, Out> pipeline_;
};

// =============================================================================
// MessageBusSource<T> — MessageBus subscription → Pipeline Source bridge
// =============================================================================

/**
 * @brief Adapter that connects a MessageBus topic subscription as a Pipeline Source.
 *
 * Used with `PipelineBuilder::with_source()` to feed messages published to a
 * MessageBus topic directly into a pipeline's input.
 *
 * ### Usage example
 * ```cpp
 * auto pipeline = PipelineBuilder<OrderEvent, OrderEvent>{}
 *     .with_source(MessageBusSource<OrderEvent>(bus, "orders"))
 *     .add<ProcessedOrder>(process_fn)
 *     .build();
 * pipeline.start(dispatcher);
 * ```
 *
 * @tparam T Message type.
 */
template <typename T>
class MessageBusSource {
public:
    /**
     * @brief Constructs a MessageBusSource.
     *
     * @param bus   MessageBus instance to connect to (must outlive this Source).
     * @param topic Topic name to subscribe to.
     * @param cap   Internal stream channel capacity (default 256).
     */
    MessageBusSource(MessageBus& bus, std::string topic, size_t cap = 256)
        : bus_(bus), topic_(std::move(topic)), cap_(cap) {}

    /**
     * @brief Calls subscribe_stream<T>() to initialize the stream channel.
     * @returns Always `Result<void>{}`.
     */
    Result<void> init() noexcept {
        stream_ch_ = bus_.subscribe_stream<T>(topic_, cap_);
        return Result<void>{};
    }

    /**
     * @brief Reads the next message from the stream channel.
     *
     * @returns A message pointer, or nullopt if the channel is closed.
     *          The returned pointer is only valid until the next call to next().
     */
    Task<std::optional<const T*>> next() {
        auto val = co_await stream_ch_->recv();
        if (!val) co_return std::nullopt;
        buf_ = std::move(*val);
        co_return &buf_;
    }

    /**
     * @brief Closes the stream channel to terminate the pipeline pump.
     */
    void close() {
        if (stream_ch_) stream_ch_->close();
    }

private:
    MessageBus&                      bus_;
    std::string                      topic_;
    size_t                           cap_;
    std::shared_ptr<AsyncChannel<T>> stream_ch_;
    T                                buf_{};  ///< Buffer holding the received value (for pointer return)
};

// =============================================================================
// MessageBusSink<T> — Pipeline tail → MessageBus publish bridge
// =============================================================================

/**
 * @brief Sink adapter that publishes pipeline output to a MessageBus topic.
 *
 * Used with `PipelineBuilder::with_sink()` to automatically publish the last
 * stage's output to a MessageBus topic.
 *
 * ### Usage example
 * ```cpp
 * auto pipeline = PipelineBuilder<RawEvent, ProcessedEvent>{}
 *     .add<ProcessedEvent>(process_fn)
 *     .with_sink(MessageBusSink<ProcessedEvent>(bus, "processed_events"))
 *     .build();
 * pipeline.start(dispatcher);
 * ```
 *
 * @tparam T Message type.
 */
template <typename T>
class MessageBusSink {
public:
    /**
     * @brief Constructs a MessageBusSink.
     *
     * @param bus   MessageBus instance to publish to (must outlive this Sink).
     * @param topic Topic name to publish to.
     */
    MessageBusSink(MessageBus& bus, std::string topic)
        : bus_(bus), topic_(std::move(topic)) {}

    /** @brief Initialization (no-op). */
    Result<void> init() noexcept { return Result<void>{}; }

    /**
     * @brief Publishes a message received from the pipeline to the MessageBus topic.
     *
     * @param msg Message to publish.
     * @returns Whether the publish succeeded.
     */
    Task<Result<void>> sink(const T& msg) {
        return bus_.publish(topic_, msg);
    }

private:
    MessageBus&  bus_;
    std::string  topic_;
};

} // namespace qbuem

/** @} */
