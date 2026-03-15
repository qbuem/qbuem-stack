#pragma once

/**
 * @file qbuem/pipeline/priority_channel.hpp
 * @brief 우선순위 채널 — PriorityChannel, Priority
 * @defgroup qbuem_priority_channel PriorityChannel
 * @ingroup qbuem_pipeline
 *
 * 3단계 우선순위(High, Normal, Low)를 가진 MPMC 비동기 채널 구현입니다.
 * 각 우선순위 레벨은 별도의 AsyncChannel<T>를 사용하며,
 * 수신 시 High → Normal → Low 순서로 확인합니다.
 *
 * ## 사용 예시
 * @code
 * PriorityChannel<int> chan(128);
 * chan.try_send(42, Priority::High);
 * auto item = chan.try_recv(); // High 우선
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/async_channel.hpp>

#include <array>
#include <atomic>
#include <memory>
#include <optional>

namespace qbuem {

/**
 * @brief 채널 아이템 우선순위 열거형.
 *
 * - High   (0): 가장 높은 우선순위 — 항상 먼저 처리됩니다.
 * - Normal (1): 보통 우선순위.
 * - Low    (2): 가장 낮은 우선순위.
 */
enum class Priority { High = 0, Normal = 1, Low = 2 };

/**
 * @brief 3단계 우선순위 MPMC 채널.
 *
 * 각 우선순위 레벨별로 별도의 `AsyncChannel<T>`를 사용합니다.
 * `try_recv()` / `recv()`는 High → Normal → Low 순서로 확인합니다.
 *
 * @tparam T 전송할 값의 타입 (이동 가능해야 함).
 */
template <typename T>
class PriorityChannel {
public:
    /**
     * @brief 각 우선순위 레벨당 지정한 용량으로 채널을 생성합니다.
     *
     * @param cap_per_level 각 레벨 채널의 용량 (기본 128).
     */
    explicit PriorityChannel(size_t cap_per_level = 128) {
        for (auto& ch : channels_)
            ch = std::make_unique<AsyncChannel<T>>(cap_per_level);
    }

    // -------------------------------------------------------------------------
    // 논블로킹 send / recv
    // -------------------------------------------------------------------------

    /**
     * @brief 지정한 우선순위로 아이템을 전송합니다 (논블로킹).
     *
     * @param item 전송할 아이템.
     * @param prio 우선순위 (기본 Normal).
     * @returns 성공이면 true, 채널이 가득 차거나 닫혔으면 false.
     */
    bool try_send(T item, Priority prio = Priority::Normal) {
        if (closed_.load(std::memory_order_relaxed))
            return false;
        return channels_[static_cast<size_t>(prio)]->try_send(std::move(item));
    }

    /**
     * @brief High → Normal → Low 순서로 아이템을 꺼내려 시도합니다 (논블로킹).
     *
     * @returns 아이템이 있으면 `std::optional<T>`, 없으면 `std::nullopt`.
     */
    std::optional<T> try_recv() {
        for (auto& ch : channels_) {
            auto item = ch->try_recv();
            if (item)
                return item;
        }
        return std::nullopt;
    }

    // -------------------------------------------------------------------------
    // 비동기 send / recv (코루틴)
    // -------------------------------------------------------------------------

    /**
     * @brief 지정한 우선순위로 아이템을 전송합니다. 채널이 가득 차면 대기합니다.
     *
     * @param item 전송할 아이템.
     * @param prio 우선순위 (기본 Normal).
     * @returns `Result<void>::ok()` 또는 에러 (`errc::broken_pipe`).
     */
    Task<Result<void>> send(T item, Priority prio = Priority::Normal) {
        if (closed_.load(std::memory_order_relaxed))
            co_return unexpected(std::make_error_code(std::errc::broken_pipe));
        co_return co_await channels_[static_cast<size_t>(prio)]->send(std::move(item));
    }

    /**
     * @brief High → Normal → Low 순서로 아이템을 수신합니다.
     *
     * 모든 레벨이 비어 있으면 High 레벨에서 대기합니다.
     * 채널이 닫히고 모두 비면 `std::nullopt` (EOS) 반환.
     *
     * @returns 아이템 또는 `std::nullopt` (EOS).
     */
    Task<std::optional<T>> recv() {
        for (;;) {
            // High → Normal → Low 순서로 논블로킹 시도
            auto item = try_recv();
            if (item)
                co_return item;

            // 모두 닫히고 비어 있으면 EOS
            if (closed_.load(std::memory_order_acquire)) {
                // 다시 한 번 확인 (닫기 직전에 들어온 아이템 처리)
                item = try_recv();
                if (item)
                    co_return item;
                co_return std::nullopt;
            }

            // High 레벨 채널에서 대기 (High 도착 시 즉시 wake)
            auto high_item = co_await channels_[0]->recv();
            if (high_item)
                co_return high_item;

            // High 채널이 EOS를 반환했다면 나머지 레벨 소진
            for (size_t i = 1; i < channels_.size(); ++i) {
                auto remaining = channels_[i]->try_recv();
                if (remaining)
                    co_return remaining;
            }

            co_return std::nullopt;
        }
    }

    // -------------------------------------------------------------------------
    // 수명 주기
    // -------------------------------------------------------------------------

    /**
     * @brief 모든 레벨 채널을 닫습니다 (EOS 전파).
     */
    void close() {
        closed_.store(true, std::memory_order_release);
        for (auto& ch : channels_)
            ch->close();
    }

    /**
     * @brief 채널이 닫혔는지 확인합니다.
     */
    [[nodiscard]] bool is_closed() const noexcept {
        return closed_.load(std::memory_order_relaxed);
    }

    // -------------------------------------------------------------------------
    // 통계
    // -------------------------------------------------------------------------

    /**
     * @brief 모든 레벨에 걸친 근사 아이템 수를 반환합니다.
     */
    [[nodiscard]] size_t size_approx() const noexcept {
        size_t total = 0;
        for (const auto& ch : channels_)
            total += ch->size_approx();
        return total;
    }

    /**
     * @brief 특정 우선순위 레벨의 근사 아이템 수를 반환합니다.
     *
     * @param p 우선순위 레벨.
     */
    [[nodiscard]] size_t size_approx(Priority p) const noexcept {
        return channels_[static_cast<size_t>(p)]->size_approx();
    }

private:
    /// 각 우선순위 레벨의 AsyncChannel (0=High, 1=Normal, 2=Low)
    std::array<std::unique_ptr<AsyncChannel<T>>, 3> channels_;
    std::atomic<bool> closed_{false};
};

} // namespace qbuem

/** @} */
