#pragma once

/**
 * @file qbuem/shm/shm_bus.hpp
 * @brief SHMBus — 인트라/인터 프로세스 통합 메시지 버스.
 * @defgroup qbuem_shm_bus SHMBus
 * @ingroup qbuem_shm
 *
 * ## 개요
 * `SHMBus`는 `AsyncChannel<T>` (스레드 간)과 `SHMChannel<T>` (프로세스 간)을
 * 동일한 API로 추상화하는 통합 메시지 버스입니다.
 *
 * ## 토픽 범위 (TopicScope)
 * | Scope | 백엔드 | 레이턴시 | 설명 |
 * |-------|--------|---------|------|
 * | `LOCAL_ONLY`   | `AsyncChannel<T>` | ~50ns | 동일 프로세스 내 |
 * | `SYSTEM_WIDE`  | `SHMChannel<T>`   | ~150ns | 크로스-프로세스 |
 *
 * ## 사용 예시
 * @code
 * // 토픽 등록 (한 번만)
 * SHMBus bus;
 * bus.declare<OrderEvent>("trading.orders", TopicScope::SYSTEM_WIDE, 4096);
 *
 * // 발행
 * co_await bus.publish("trading.orders", order);
 *
 * // 구독
 * auto sub = bus.subscribe<OrderEvent>("trading.orders");
 * while (auto msg = co_await sub->recv()) {
 *     process(*msg);
 * }
 * @endcode
 *
 * ## Pipeline 통합
 * `SHMSource`와 `SHMSink`를 통해 Pipeline Stage로 사용 가능합니다:
 * @code
 * auto pipeline = PipelineBuilder<OrderEvent>()
 *     .source<SHMSource<OrderEvent>>("trading.raw_orders")
 *     .add<NormalizeAction>()
 *     .sink<SHMSink<NormalizedOrder>>("trading.normalized")
 *     .build();
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/async_channel.hpp>
#include <qbuem/shm/shm_channel.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace qbuem::shm {

// ─── TopicScope ───────────────────────────────────────────────────────────────

/** @brief 토픽의 가시성 범위를 나타냅니다. */
enum class TopicScope : uint8_t {
    LOCAL_ONLY  = 0, ///< 동일 프로세스 내 (AsyncChannel 백엔드)
    SYSTEM_WIDE = 1, ///< 크로스-프로세스 (SHMChannel 백엔드)
};

// ─── ISubscription<T> ────────────────────────────────────────────────────────

/**
 * @brief 타입-지정 구독 핸들.
 *
 * 구독은 RAII로 관리됩니다 — 소멸 시 자동으로 등록 해제.
 */
template <typename T>
class ISubscription {
public:
    virtual ~ISubscription() = default;

    /**
     * @brief 다음 메시지를 수신합니다.
     * @returns 메시지 포인터 또는 nullopt (채널 닫힘).
     */
    virtual Task<std::optional<const T*>> recv() = 0;

    /**
     * @brief 논블로킹 수신 시도.
     */
    virtual std::optional<const T*> try_recv() = 0;

    /** @brief 구독 토픽 이름. */
    [[nodiscard]] virtual std::string_view topic() const noexcept = 0;

    /** @brief 구독 범위. */
    [[nodiscard]] virtual TopicScope scope() const noexcept = 0;
};

// ─── TopicDescriptor ─────────────────────────────────────────────────────────

/**
 * @brief 토픽 메타데이터 (SHMBus 내부 저장용).
 *
 * 최대 이름 길이 63자로 제한하여 힙 할당 없이 스택 저장합니다.
 */
struct TopicDescriptor {
    static constexpr size_t kMaxNameLen = 63;

    char       name[kMaxNameLen + 1]{};
    TopicScope scope{TopicScope::LOCAL_ONLY};
    uint64_t   type_id{0};          ///< 타입 식별자 (sizeof(T) XOR typeid 기반)
    size_t     capacity{0};         ///< 링 버퍼 용량
    void*      channel_ptr{nullptr};///< AsyncChannel<T>* 또는 SHMChannel<T>* (type-erased)
    std::function<void(void*)> channel_deleter; ///< type-safe 소멸자

    /** @brief 이름을 설정합니다 (최대 63자). */
    void set_name(std::string_view n) noexcept {
        size_t len = n.size() < kMaxNameLen ? n.size() : kMaxNameLen;
        std::memcpy(name, n.data(), len);
        name[len] = '\0';
    }

    [[nodiscard]] std::string_view get_name() const noexcept { return name; }
};

// ─── SHMBus ───────────────────────────────────────────────────────────────────

/**
 * @brief 통합 메시지 버스.
 *
 * ## 내부 구조
 * - 토픽 레지스트리: 고정 배열 + 선형 탐색 (최대 `kMaxTopics` 토픽).
 * - 각 토픽은 `AsyncChannel<T>` 또는 `SHMChannel<T>`를 뒤에서 구동.
 * - 구독자는 토픽당 fan-out 방식으로 메시지를 수신.
 *
 * @note 프로세스 당 하나의 `SHMBus` 인스턴스를 사용하길 권장합니다.
 */
class SHMBus {
public:
    static constexpr size_t kMaxTopics = 64;

    SHMBus() = default;

    ~SHMBus() {
        size_t n = topic_count_.load(std::memory_order_acquire);
        for (size_t i = 0; i < n; ++i) {
            auto& desc = topics_[i];
            if (desc.channel_ptr && desc.channel_deleter) {
                desc.channel_deleter(desc.channel_ptr);
                desc.channel_ptr = nullptr;
            }
        }
    }

    SHMBus(const SHMBus&) = delete;
    SHMBus& operator=(const SHMBus&) = delete;

    // ── 토픽 선언 ──────────────────────────────────────────────────────────

    /**
     * @brief 타입 `T`의 토픽을 버스에 등록합니다.
     *
     * - `LOCAL_ONLY`: `AsyncChannel<T>` 생성.
     * - `SYSTEM_WIDE`: `SHMChannel<T>` 생성 (memfd 세그먼트).
     *
     * @tparam T     메시지 타입.
     * @param name   토픽 이름 (e.g. "trading.orders").
     * @param scope  LOCAL_ONLY | SYSTEM_WIDE.
     * @param cap    링 버퍼 용량.
     * @returns 성공이면 true, 이미 등록되었거나 한도 초과이면 false.
     */
    template <typename T>
    bool declare(std::string_view name, TopicScope scope, size_t cap = 1024) {
        static_assert(std::is_trivially_copyable_v<T> || scope == TopicScope::LOCAL_ONLY,
                      "SHMChannel requires trivially_copyable T");
        size_t idx = topic_count_.load(std::memory_order_relaxed);
        if (idx >= kMaxTopics) return false;

        // 중복 검사
        if (find_topic(name) != nullptr) return false;

        auto& desc = topics_[idx];
        desc.set_name(name);
        desc.scope    = scope;
        desc.type_id  = make_type_id<T>();
        desc.capacity = cap;

        if (scope == TopicScope::LOCAL_ONLY) {
            desc.channel_ptr    = new AsyncChannel<T>(cap);
            desc.channel_deleter = [](void* p) {
                delete static_cast<AsyncChannel<T>*>(p);
            };
        } else {
            // SYSTEM_WIDE: SHMChannel 생성 (생산자 측)
            auto res = SHMChannel<T>::create(name, cap);
            if (!res) return false;
            desc.channel_ptr    = res->release();
            desc.channel_deleter = [](void* p) {
                delete static_cast<SHMChannel<T>*>(p);
            };
        }

        topic_count_.fetch_add(1, std::memory_order_release);
        return true;
    }

    // ── 발행 ────────────────────────────────────────────────────────────────

    /**
     * @brief 토픽에 메시지를 발행합니다.
     *
     * @tparam T     메시지 타입.
     * @param name   토픽 이름.
     * @param msg    전송할 메시지.
     * @returns 성공 또는 에러 (토픽 미등록, 타입 불일치, 채널 에러).
     */
    template <typename T>
    Task<Result<void>> publish(std::string_view name, const T& msg) {
        auto* desc = find_topic(name);
        if (!desc) co_return unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
        if (desc->type_id != make_type_id<T>()) co_return unexpected(std::make_error_code(std::errc::invalid_argument));

        if (desc->scope == TopicScope::LOCAL_ONLY) {
            auto* ch = static_cast<AsyncChannel<T>*>(desc->channel_ptr);
            co_return co_await ch->send(T(msg));
        } else {
            auto* ch = static_cast<SHMChannel<T>*>(desc->channel_ptr);
            co_return co_await ch->send(msg);
        }
    }

    /**
     * @brief 논블로킹 발행 시도.
     */
    template <typename T>
    bool try_publish(std::string_view name, const T& msg) {
        auto* desc = find_topic(name);
        if (!desc || desc->type_id != make_type_id<T>()) return false;

        if (desc->scope == TopicScope::LOCAL_ONLY) {
            auto* ch = static_cast<AsyncChannel<T>*>(desc->channel_ptr);
            return ch->try_send(T(msg));
        } else {
            auto* ch = static_cast<SHMChannel<T>*>(desc->channel_ptr);
            return ch->try_send(msg);
        }
    }

    // ── 구독 ────────────────────────────────────────────────────────────────

    /**
     * @brief 토픽을 구독합니다.
     *
     * @tparam T   메시지 타입.
     * @param name 토픽 이름.
     * @returns 구독 핸들 또는 nullptr (토픽 미등록/타입 불일치).
     */
    template <typename T>
    std::unique_ptr<ISubscription<T>> subscribe(std::string_view name) {
        auto* desc = find_topic(name);
        if (!desc || desc->type_id != make_type_id<T>()) return nullptr;

        if (desc->scope == TopicScope::LOCAL_ONLY) {
            auto* ch = static_cast<AsyncChannel<T>*>(desc->channel_ptr);
            return std::make_unique<LocalSub<T>>(ch, name);
        } else {
            auto* ch = static_cast<SHMChannel<T>*>(desc->channel_ptr);
            return std::make_unique<SystemSub<T>>(ch, name);
        }
    }

    // ── 유틸리티 ────────────────────────────────────────────────────────────

    /** @brief 등록된 토픽 수를 반환합니다. */
    [[nodiscard]] size_t topic_count() const noexcept {
        return topic_count_.load(std::memory_order_relaxed);
    }

    /** @brief 토픽이 등록되어 있는지 확인합니다. */
    [[nodiscard]] bool has_topic(std::string_view name) const noexcept {
        return find_topic(name) != nullptr;
    }

private:
    // ── 내부 구독 구현 ──────────────────────────────────────────────────────

    template <typename T>
    struct LocalSub final : ISubscription<T> {
        AsyncChannel<T>* ch;
        const char*      name_str;
        T                buf_{};  ///< per-subscriber buffer (avoids static thread_local aliasing)

        LocalSub(AsyncChannel<T>* c, std::string_view n)
            : ch(c), name_str(n.data()) {}

        Task<std::optional<const T*>> recv() override {
            auto item = co_await ch->recv();
            if (!item) co_return std::nullopt;
            buf_ = std::move(*item);
            co_return &buf_;
        }

        std::optional<const T*> try_recv() override {
            auto item = ch->try_recv();
            if (!item) return std::nullopt;
            buf_ = std::move(*item);
            return &buf_;
        }

        [[nodiscard]] std::string_view topic() const noexcept override { return name_str; }
        [[nodiscard]] TopicScope scope() const noexcept override { return TopicScope::LOCAL_ONLY; }
    };

    template <typename T>
    struct SystemSub final : ISubscription<T> {
        SHMChannel<T>* ch;
        const char*    name_str;

        SystemSub(SHMChannel<T>* c, std::string_view n)
            : ch(c), name_str(n.data()) {}

        Task<std::optional<const T*>> recv() override { return ch->recv(); }
        std::optional<const T*> try_recv() override { return ch->try_recv(); }
        [[nodiscard]] std::string_view topic() const noexcept override { return name_str; }
        [[nodiscard]] TopicScope scope() const noexcept override { return TopicScope::SYSTEM_WIDE; }
    };

    // ── 토픽 레지스트리 ────────────────────────────────────────────────────

    TopicDescriptor*       find_topic(std::string_view name) noexcept {
        size_t n = topic_count_.load(std::memory_order_acquire);
        for (size_t i = 0; i < n; ++i) {
            if (topics_[i].get_name() == name) return &topics_[i];
        }
        return nullptr;
    }

    const TopicDescriptor* find_topic(std::string_view name) const noexcept {
        size_t n = topic_count_.load(std::memory_order_acquire);
        for (size_t i = 0; i < n; ++i) {
            if (topics_[i].get_name() == name) return &topics_[i];
        }
        return nullptr;
    }

    /** @brief 타입 ID 생성 (sizeof + alignof 기반, 충돌 위험 낮음). */
    template <typename T>
    static constexpr uint64_t make_type_id() noexcept {
        return (uint64_t(sizeof(T)) << 32) | uint64_t(alignof(T));
    }

    TopicDescriptor      topics_[kMaxTopics]{};
    std::atomic<size_t>  topic_count_{0};
};

// ─── Pipeline 통합: SHMSource / SHMSink ──────────────────────────────────────

/**
 * @brief SHM 채널에서 메시지를 읽는 Pipeline Source 액션.
 *
 * @tparam T 메시지 타입.
 *
 * @code
 * // Pipeline Head에서 SHM 채널 소비
 * auto pipeline = PipelineBuilder<MyMsg>()
 *     .source<SHMSource<MyMsg>>("system.ingress")
 *     .add<ProcessAction>()
 *     .build();
 * @endcode
 */
template <typename T>
class SHMSource {
public:
    explicit SHMSource(std::string_view channel_name)
        : name_(channel_name) {}

    /**
     * @brief 채널을 초기화하고 엽니다.
     * @returns 성공 또는 에러.
     */
    Result<void> init() noexcept {
        auto res = SHMChannel<T>::open(name_);
        if (!res) return unexpected(res.error());
        channel_ = std::move(*res);
        return Result<void>::ok();
    }

    /**
     * @brief 다음 메시지를 비동기적으로 읽습니다.
     * @returns 메시지 포인터 (zero-copy) 또는 nullopt.
     */
    Task<std::optional<const T*>> next() {
        return channel_->recv();
    }

private:
    std::string_view              name_;
    typename SHMChannel<T>::Ptr   channel_;
};

/**
 * @brief SHM 채널로 메시지를 쓰는 Pipeline Sink 액션.
 *
 * @tparam T 메시지 타입.
 *
 * @code
 * auto pipeline = PipelineBuilder<ProcessedMsg>()
 *     .add<TransformAction>()
 *     .sink<SHMSink<ProcessedMsg>>("system.analytics")
 *     .build();
 * @endcode
 */
template <typename T>
class SHMSink {
public:
    explicit SHMSink(std::string_view channel_name, size_t capacity = 1024)
        : name_(channel_name), capacity_(capacity) {}

    /**
     * @brief 채널을 초기화하고 생성합니다.
     * @returns 성공 또는 에러.
     */
    Result<void> init() noexcept {
        auto res = SHMChannel<T>::create(name_, capacity_);
        if (!res) return unexpected(res.error());
        channel_ = std::move(*res);
        return Result<void>::ok();
    }

    /**
     * @brief 메시지를 SHM 채널로 전송합니다.
     * 슬롯이 가득 차면 co_await 백프레셔.
     * @param msg 전송할 메시지.
     */
    Task<Result<void>> sink(const T& msg) {
        return channel_->send(msg);
    }

private:
    std::string_view              name_;
    size_t                        capacity_;
    typename SHMChannel<T>::Ptr   channel_;
};

} // namespace qbuem::shm

/** @} */
