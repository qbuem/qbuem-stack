#pragma once

/**
 * @file qbuem/pipeline/message_bus.hpp
 * @brief 토픽 기반 발행/구독 메시지 버스 — MessageBus, SubpipelineAction
 * @defgroup qbuem_message_bus MessageBus
 * @ingroup qbuem_pipeline
 *
 * MessageBus는 토픽 기반 pub/sub 패턴과 스트리밍을 지원합니다.
 * 4가지 패턴을 지원합니다:
 * - Unary (1→1): 단일 발행자 → 단일 구독자 (round-robin)
 * - ServerStream (1→N): 단일 발행, 스트림 채널 수신
 * - ClientStream (N→1): 채널 누적 후 단일 결과
 * - Bidi: 양방향 스트리밍
 *
 * ## 사용 예시
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

/**
 * @brief 토픽 기반 발행/구독 메시지 버스.
 *
 * 메시지는 std::any로 타입 소거되어 전달됩니다.
 * 구독 핸들(Subscription)의 소멸자가 구독을 자동으로 취소합니다.
 */
class MessageBus {
public:
    /// @brief 타입 소거된 메시지 타입.
    using Msg = std::any;

    /// @brief 메시지 핸들러 타입.
    using Handler = std::function<Task<Result<void>>(Msg, Context)>;

    // -------------------------------------------------------------------------
    // Subscription 핸들
    // -------------------------------------------------------------------------

    /**
     * @brief 구독 핸들 — 소멸 시 자동으로 구독 취소.
     *
     * RAII 방식으로 구독 수명을 관리합니다.
     * 이동 가능하지만 복사 불가능합니다.
     */
    class Subscription {
    public:
        Subscription() = default;

        /**
         * @brief 구독 핸들을 생성합니다.
         * @param topic 구독 토픽.
         * @param id    구독 ID.
         * @param bus   소유 MessageBus 포인터.
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

        /// @brief 구독을 명시적으로 취소합니다.
        void cancel() {
            if (active_ && bus_) {
                bus_->unsubscribe(topic_, id_);
                active_ = false;
                bus_    = nullptr;
            }
        }

        /// @brief 구독이 활성화되어 있는지 확인합니다.
        [[nodiscard]] bool is_active() const noexcept { return active_; }

    private:
        std::string  topic_;
        size_t       id_     = 0;
        MessageBus*  bus_    = nullptr;
        bool         active_ = false;
    };

    // -------------------------------------------------------------------------
    // 생성자 / 소멸자
    // -------------------------------------------------------------------------

    MessageBus() = default;
    ~MessageBus() = default;

    MessageBus(const MessageBus&) = delete;
    MessageBus& operator=(const MessageBus&) = delete;

    // -------------------------------------------------------------------------
    // 구독
    // -------------------------------------------------------------------------

    /**
     * @brief Unary 패턴 구독 — 메시지가 핸들러로 직접 전달됩니다.
     *
     * 같은 토픽에 여러 구독자가 있으면 fan-out (모두 수신).
     *
     * @param topic    구독할 토픽.
     * @param handler  메시지 처리 함수.
     * @param chan_cap 내부 채널 용량.
     * @returns 구독 핸들 (소멸 시 자동 취소).
     */
    Subscription subscribe(std::string topic, Handler handler, size_t chan_cap = 64) {
        auto state = get_or_create_topic(topic);
        size_t id = state->next_id.fetch_add(1, std::memory_order_relaxed);

        Sub sub;
        sub.id      = id;
        sub.handler = std::move(handler);
        sub.channel = std::make_shared<AsyncChannel<Msg>>(chan_cap);

        {
            std::unique_lock lock(state->mtx);
            state->subs.push_back(std::move(sub));
        }

        return Subscription(topic, id, this);
    }

    /**
     * @brief 메시지를 토픽의 모든 구독자에게 발행합니다 (fan-out, backpressure).
     *
     * @tparam T 메시지 타입.
     * @param topic 발행할 토픽.
     * @param msg   발행할 메시지.
     * @param ctx   메시지 컨텍스트.
     * @returns Result<void>::ok() 또는 에러.
     */
    template <typename T>
    Task<Result<void>> publish(std::string_view topic, T msg, Context ctx = {}) {
        std::shared_ptr<TopicState> state;
        {
            std::shared_lock lock(topics_mtx_);
            auto it = topics_.find(std::string(topic));
            if (it == topics_.end())
                co_return Result<void>{};
            state = it->second;
        }

        if (state->closed.load(std::memory_order_acquire))
            co_return unexpected(std::make_error_code(std::errc::broken_pipe));

        Msg any_msg = std::move(msg);

        std::vector<std::shared_ptr<AsyncChannel<Msg>>> channels;
        {
            std::shared_lock lock(state->mtx);
            for (auto& sub : state->subs) {
                if (sub.channel)
                    channels.push_back(sub.channel);
            }
        }

        for (auto& ch : channels) {
            auto result = co_await ch->send(any_msg);
            if (!result)
                co_return result;
        }
        co_return Result<void>{};
    }

    /**
     * @brief 논블로킹 발행 (채널이 가득 차면 드롭).
     *
     * @tparam T 메시지 타입.
     * @param topic 발행할 토픽.
     * @param msg   발행할 메시지.
     * @param ctx   메시지 컨텍스트.
     * @returns 모든 구독자에게 성공적으로 전송 시 true.
     */
    template <typename T>
    bool try_publish(std::string_view topic, T msg, Context /*ctx*/ = {}) {
        std::shared_ptr<TopicState> state;
        {
            std::shared_lock lock(topics_mtx_);
            auto it = topics_.find(std::string(topic));
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
            if (sub.channel && !sub.channel->try_send(any_msg))
                all_ok = false;
        }
        return all_ok;
    }

    /**
     * @brief ServerStream 구독 — 채널을 통해 스트림을 수신합니다.
     *
     * 반환된 채널은 토픽에 발행된 메시지를 스트리밍합니다.
     * 토픽이 닫히면 채널도 닫힙니다.
     *
     * @tparam T 메시지 타입.
     * @param topic 구독할 토픽.
     * @param cap   채널 용량.
     * @returns 메시지 스트림 채널.
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
        sub.channel = std::make_shared<AsyncChannel<Msg>>(cap);
        // Store the typed channel alongside the sub
        auto typed_ch = stream_ch;
        sub.handler = [typed_ch](Msg m, Context) -> Task<Result<void>> {
            if (const T* p = std::any_cast<T>(&m)) {
                co_await typed_ch->send(*p);
            }
            co_return Result<void>{};
        };

        {
            std::unique_lock lock(state->mtx);
            state->subs.push_back(std::move(sub));
        }

        return stream_ch;
    }

    /**
     * @brief ClientStream 구독 — 채널에서 누적 후 단일 결과를 생성합니다.
     *
     * @tparam In  입력 메시지 타입.
     * @tparam Out 출력 결과 타입.
     * @param topic  구독할 토픽.
     * @param fn     누적 함수: AsyncChannel<In> -> Task<Result<Out>>.
     * @param cap    내부 채널 용량.
     * @returns 구독 핸들.
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
     * @brief 토픽을 닫습니다 — 모든 구독자에게 EOS 전파.
     *
     * @param topic 닫을 토픽 이름.
     */
    void close_topic(std::string_view topic) {
        std::shared_ptr<TopicState> state;
        {
            std::shared_lock lock(topics_mtx_);
            auto it = topics_.find(std::string(topic));
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
    // 통계
    // -------------------------------------------------------------------------

    /**
     * @brief 특정 토픽의 구독자 수를 반환합니다.
     */
    [[nodiscard]] size_t subscriber_count(std::string_view topic) const {
        std::shared_lock outer(topics_mtx_);
        auto it = topics_.find(std::string(topic));
        if (it == topics_.end())
            return 0;
        std::shared_lock inner(it->second->mtx);
        return it->second->subs.size();
    }

    /**
     * @brief 현재 존재하는 모든 토픽 이름을 반환합니다.
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
    // 라이프사이클
    // -------------------------------------------------------------------------

    /**
     * @brief MessageBus를 시작합니다.
     *
     * @param dispatcher 내부 워커를 실행할 Dispatcher.
     */
    void start(Dispatcher& dispatcher) {
        dispatcher_ = &dispatcher;
    }

    /**
     * @brief 모든 토픽을 닫고 드레인합니다.
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
    // 내부 구조체
    // -------------------------------------------------------------------------

    struct Sub {
        size_t                                   id;
        Handler                                  handler;
        std::shared_ptr<AsyncChannel<Msg>>       channel;
    };

    struct TopicState {
        mutable std::shared_mutex      mtx;
        std::vector<Sub>               subs;
        std::atomic<size_t>            next_id{1};
        std::atomic<bool>              closed{false};
    };

    // -------------------------------------------------------------------------
    // 내부 헬퍼
    // -------------------------------------------------------------------------

    /**
     * @brief 토픽 상태를 반환합니다. 없으면 생성합니다.
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

    /**
     * @brief 특정 토픽에서 ID에 해당하는 구독을 제거합니다.
     */
    void unsubscribe(std::string_view topic, size_t id) {
        std::shared_ptr<TopicState> state;
        {
            std::shared_lock lock(topics_mtx_);
            auto it = topics_.find(std::string(topic));
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
    // 데이터 멤버
    // -------------------------------------------------------------------------
    mutable std::shared_mutex                                    topics_mtx_;
    std::unordered_map<std::string, std::shared_ptr<TopicState>> topics_;
    Dispatcher*                                                  dispatcher_ = nullptr;
};

// =============================================================================
// SubpipelineAction
// =============================================================================

/**
 * @brief StaticPipeline<In,Out>을 단일 Action<In,Out>으로 래핑합니다.
 *
 * DynamicPipeline 스테이지로 StaticPipeline을 내장할 때 사용합니다.
 *
 * @tparam In  입력 타입.
 * @tparam Out 출력 타입.
 */
template <typename In, typename Out>
class SubpipelineAction {
public:
    /**
     * @brief StaticPipeline으로 SubpipelineAction을 생성합니다.
     *
     * @param pipeline 이미 스테이지가 추가된 StaticPipeline.
     */
    explicit SubpipelineAction(StaticPipeline<In, Out> pipeline)
        : pipeline_(std::move(pipeline))
    {}

    /**
     * @brief 아이템을 내부 파이프라인에 전송합니다 (backpressure).
     */
    Task<Result<void>> push(In item, Context ctx = {}) {
        return pipeline_.push(std::move(item), std::move(ctx));
    }

    /**
     * @brief 논블로킹 push.
     */
    bool try_push(In item, Context ctx = {}) {
        return pipeline_.try_push(std::move(item), std::move(ctx));
    }

    /**
     * @brief 내부 파이프라인을 시작합니다.
     *
     * @param dispatcher 워커를 실행할 Dispatcher.
     * @param out        출력 채널 (nullptr이면 파이프라인의 내부 출력 사용).
     */
    void start(Dispatcher& dispatcher,
               std::shared_ptr<AsyncChannel<ContextualItem<Out>>> out = nullptr) {
        (void)out; // StaticPipeline manages its own tail channel
        pipeline_.start(dispatcher);
    }

    /**
     * @brief 드레인: 입력을 닫고 모든 처리가 완료될 때까지 대기.
     */
    Task<void> drain() {
        return pipeline_.drain();
    }

    /**
     * @brief 즉시 정지합니다.
     */
    void stop() {
        pipeline_.stop();
    }

    /**
     * @brief 내부 파이프라인의 출력 채널을 반환합니다.
     */
    [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<Out>>> output() const {
        return pipeline_.output();
    }

private:
    StaticPipeline<In, Out> pipeline_;
};

// =============================================================================
// MessageBusSource<T> — MessageBus 구독 → Pipeline Source 브릿지
// =============================================================================

/**
 * @brief MessageBus 토픽 구독을 Pipeline Source로 연결하는 어댑터.
 *
 * `PipelineBuilder::with_source()`와 함께 사용하여 MessageBus 토픽에서
 * 발행된 메시지를 파이프라인의 입력으로 직접 흘릴 수 있습니다.
 *
 * ### 사용 예시
 * ```cpp
 * auto pipeline = PipelineBuilder<OrderEvent, OrderEvent>{}
 *     .with_source(MessageBusSource<OrderEvent>(bus, "orders"))
 *     .add<ProcessedOrder>(process_fn)
 *     .build();
 * pipeline.start(dispatcher);
 * ```
 *
 * @tparam T 메시지 타입.
 */
template <typename T>
class MessageBusSource {
public:
    /**
     * @brief MessageBusSource를 생성합니다.
     *
     * @param bus   연결할 MessageBus 인스턴스 (수명이 Source보다 길어야 함).
     * @param topic 구독할 토픽 이름.
     * @param cap   내부 스트림 채널 용량 (기본 256).
     */
    MessageBusSource(MessageBus& bus, std::string topic, size_t cap = 256)
        : bus_(bus), topic_(std::move(topic)), cap_(cap) {}

    /**
     * @brief subscribe_stream<T>()를 호출해 스트림 채널을 초기화합니다.
     * @returns 항상 `Result<void>::ok()`.
     */
    Result<void> init() noexcept {
        stream_ch_ = bus_.subscribe_stream<T>(topic_, cap_);
        return Result<void>::ok();
    }

    /**
     * @brief 스트림 채널에서 다음 메시지를 읽습니다.
     *
     * @returns 메시지 포인터 또는 nullopt (채널 닫힘).
     *          반환된 포인터는 다음 next() 호출 전까지만 유효합니다.
     */
    Task<std::optional<const T*>> next() {
        auto val = co_await stream_ch_->recv();
        if (!val) co_return std::nullopt;
        buf_ = std::move(*val);
        co_return &buf_;
    }

    /**
     * @brief 스트림 채널을 닫아 파이프라인 펌프를 종료합니다.
     */
    void close() {
        if (stream_ch_) stream_ch_->close();
    }

private:
    MessageBus&                      bus_;
    std::string                      topic_;
    size_t                           cap_;
    std::shared_ptr<AsyncChannel<T>> stream_ch_;
    T                                buf_{};  ///< recv된 값 보관 버퍼 (포인터 반환용)
};

// =============================================================================
// MessageBusSink<T> — Pipeline Tail → MessageBus 발행 브릿지
// =============================================================================

/**
 * @brief Pipeline 출력을 MessageBus 토픽으로 발행하는 싱크 어댑터.
 *
 * `PipelineBuilder::with_sink()`와 함께 사용하여 파이프라인의 마지막 단계
 * 출력을 MessageBus 토픽으로 자동 발행할 수 있습니다.
 *
 * ### 사용 예시
 * ```cpp
 * auto pipeline = PipelineBuilder<RawEvent, ProcessedEvent>{}
 *     .add<ProcessedEvent>(process_fn)
 *     .with_sink(MessageBusSink<ProcessedEvent>(bus, "processed_events"))
 *     .build();
 * pipeline.start(dispatcher);
 * ```
 *
 * @tparam T 메시지 타입.
 */
template <typename T>
class MessageBusSink {
public:
    /**
     * @brief MessageBusSink를 생성합니다.
     *
     * @param bus   발행할 MessageBus 인스턴스 (수명이 Sink보다 길어야 함).
     * @param topic 발행할 토픽 이름.
     */
    MessageBusSink(MessageBus& bus, std::string topic)
        : bus_(bus), topic_(std::move(topic)) {}

    /** @brief 초기화 (no-op). */
    Result<void> init() noexcept { return Result<void>::ok(); }

    /**
     * @brief 파이프라인에서 받은 메시지를 MessageBus 토픽으로 발행합니다.
     *
     * @param msg 발행할 메시지.
     * @returns 발행 성공 여부.
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
