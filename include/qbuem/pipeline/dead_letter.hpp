#pragma once

/**
 * @file qbuem/pipeline/dead_letter.hpp
 * @brief 데드 레터 큐 — DeadLetter, DeadLetterQueue, DlqAction
 * @defgroup qbuem_dead_letter DeadLetterQueue
 * @ingroup qbuem_pipeline
 *
 * 처리 실패한 아이템을 수집하여 나중에 재처리하거나 감사(audit)할 수 있는
 * 데드 레터 큐(Dead Letter Queue) 구현입니다.
 *
 * ## 사용 예시
 * @code
 * auto dlq = std::make_shared<DeadLetterQueue<int>>();
 * DlqAction<int, int> action(my_fn, dlq, 3);
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action_env.hpp>

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <system_error>
#include <vector>

namespace qbuem {

/**
 * @brief 데드 레터 엔트리 — 실패한 아이템 + 에러 + 컨텍스트 + 시도 횟수.
 *
 * @tparam T 아이템 타입.
 */
template <typename T>
struct DeadLetter {
    T item;                                                               ///< 실패한 아이템
    Context ctx;                                                          ///< 아이템 컨텍스트
    std::error_code error;                                                ///< 실패 에러 코드
    size_t attempt_count = 1;                                             ///< 총 시도 횟수
    std::chrono::system_clock::time_point failed_at =
        std::chrono::system_clock::now();                                 ///< 실패 시각
};

/**
 * @brief 데드 레터 큐 — 실패한 아이템을 수집하고 재처리를 지원합니다.
 *
 * 스레드 안전합니다. max_size 초과 시 가장 오래된 항목을 드롭합니다.
 *
 * @tparam T 아이템 타입.
 */
template <typename T>
class DeadLetterQueue {
public:
    /**
     * @brief 데드 레터 큐 설정.
     */
    struct Config {
        size_t max_size   = 10000; ///< 최대 항목 수 (초과 시 가장 오래된 항목 드롭)
        bool   persist_log = false; ///< true이면 각 항목을 AsyncLogger에 기록
    };

    /**
     * @brief 지정한 설정으로 DeadLetterQueue를 생성합니다.
     * @param cfg 설정 (기본값 사용 가능).
     */
    explicit DeadLetterQueue(Config cfg = {})
        : cfg_(std::move(cfg)) {}

    /**
     * @brief 데드 레터를 추가합니다 (스레드 안전).
     *
     * max_size 초과 시 가장 오래된 항목을 드롭합니다.
     *
     * @param letter 추가할 데드 레터.
     */
    void push(DeadLetter<T> letter) {
        std::lock_guard lock(mtx_);
        // max_size 초과 시 가장 오래된 항목(front) 드롭
        if (cfg_.max_size > 0 && queue_.size() >= cfg_.max_size) {
            queue_.pop_front();
        }
        queue_.push_back(std::move(letter));
    }

    /**
     * @brief 아이템과 에러 정보로 데드 레터를 추가합니다 (스레드 안전).
     *
     * @param item     실패한 아이템.
     * @param ctx      아이템 컨텍스트.
     * @param err      실패 에러 코드.
     * @param attempts 총 시도 횟수 (기본 1).
     */
    void push(T item, Context ctx, std::error_code err, size_t attempts = 1) {
        push(DeadLetter<T>{
            std::move(item),
            std::move(ctx),
            err,
            attempts,
            std::chrono::system_clock::now()
        });
    }

    /**
     * @brief 모든 데드 레터를 꺼냅니다 (이동). 큐를 비웁니다.
     *
     * @returns 모든 데드 레터의 벡터.
     */
    std::vector<DeadLetter<T>> drain() {
        std::lock_guard lock(mtx_);
        std::vector<DeadLetter<T>> result;
        result.reserve(queue_.size());
        for (auto& letter : queue_)
            result.push_back(std::move(letter));
        queue_.clear();
        return result;
    }

    /**
     * @brief 데드 레터를 소비하지 않고 조회합니다.
     *
     * @param max_n 최대 조회 개수 (기본 100).
     * @returns 데드 레터 포인터 벡터 (const).
     */
    std::vector<const DeadLetter<T>*> peek(size_t max_n = 100) const {
        std::lock_guard lock(mtx_);
        std::vector<const DeadLetter<T>*> result;
        size_t n = std::min(max_n, queue_.size());
        result.reserve(n);
        auto it = queue_.begin();
        for (size_t i = 0; i < n; ++i, ++it)
            result.push_back(&(*it));
        return result;
    }

    /**
     * @brief 데드 레터를 재처리합니다.
     *
     * fn을 각 항목에 적용하며, 성공 시 큐에서 제거합니다.
     * 실패한 항목은 큐에 유지됩니다.
     *
     * @param fn    재처리 함수.
     * @param max_n 최대 재처리 개수 (기본 SIZE_MAX = 전체).
     * @returns 성공적으로 재처리된 항목 수.
     */
    Task<size_t> reprocess(
        std::function<Task<Result<void>>(T, Context)> fn,
        size_t max_n = SIZE_MAX)
    {
        // 현재 항목들을 스냅샷으로 꺼낸 뒤 처리
        std::vector<DeadLetter<T>> snapshot;
        {
            std::lock_guard lock(mtx_);
            size_t n = std::min(max_n, queue_.size());
            snapshot.reserve(n);
            auto it = queue_.begin();
            for (size_t i = 0; i < n; ++i, ++it)
                snapshot.push_back(std::move(*it));
            queue_.erase(queue_.begin(), queue_.begin() + static_cast<ptrdiff_t>(snapshot.size()));
        }

        size_t success_count = 0;
        std::vector<DeadLetter<T>> failed;

        for (auto& letter : snapshot) {
            auto result = co_await fn(letter.item, letter.ctx);
            if (result.has_value()) {
                ++success_count;
            } else {
                // 실패한 항목은 재삽입
                letter.error = result.error();
                ++letter.attempt_count;
                failed.push_back(std::move(letter));
            }
        }

        // 실패 항목 재삽입 (순서 유지: failed가 앞에 오도록)
        if (!failed.empty()) {
            std::lock_guard lock(mtx_);
            for (auto it = failed.rbegin(); it != failed.rend(); ++it)
                queue_.push_front(std::move(*it));
        }

        co_return success_count;
    }

    /**
     * @brief 현재 큐의 크기를 반환합니다.
     */
    size_t size() const noexcept {
        std::lock_guard lock(mtx_);
        return queue_.size();
    }

    /**
     * @brief 큐가 비어 있는지 확인합니다.
     */
    bool empty() const noexcept {
        std::lock_guard lock(mtx_);
        return queue_.empty();
    }

    /**
     * @brief 큐를 비웁니다 (스레드 안전).
     */
    void clear() noexcept {
        std::lock_guard lock(mtx_);
        queue_.clear();
    }

private:
    Config                    cfg_;
    mutable std::mutex        mtx_;
    std::deque<DeadLetter<T>> queue_;
};

/**
 * @brief DLQ 인식 액션 래퍼.
 *
 * 내부 액션 실패 시 아이템을 DeadLetterQueue로 전송합니다.
 * max_attempts 초과 후에만 DLQ로 전송합니다.
 *
 * @tparam In  입력 타입.
 * @tparam Out 출력 타입.
 */
template <typename In, typename Out>
class DlqAction {
public:
    /** @brief 내부 액션 함수 타입. */
    using InnerFn = std::function<Task<Result<Out>>(In, ActionEnv)>;

    /**
     * @brief DlqAction을 생성합니다.
     * @param fn           감쌀 내부 액션 함수.
     * @param dlq          데드 레터 큐 (공유 포인터).
     * @param max_attempts DLQ로 보내기 전 최대 시도 횟수 (기본 3).
     */
    DlqAction(InnerFn fn,
              std::shared_ptr<DeadLetterQueue<In>> dlq,
              size_t max_attempts = 3)
        : fn_(std::move(fn))
        , dlq_(std::move(dlq))
        , max_attempts_(max_attempts) {}

    /**
     * @brief DLQ 보호 하에 액션을 실행합니다.
     *
     * max_attempts 이후에도 실패하면 아이템을 DLQ로 전송합니다.
     *
     * @param item 처리할 아이템.
     * @param env  실행 환경.
     * @returns Task<Result<Out>>
     */
    Task<Result<Out>> operator()(In item, ActionEnv env) {
        Result<Out> last_result = unexpected(std::make_error_code(std::errc::operation_not_permitted));

        for (size_t attempt = 0; attempt < max_attempts_; ++attempt) {
            if (env.stop.stop_requested()) {
                co_return unexpected(std::make_error_code(std::errc::operation_canceled));
            }

            auto result = co_await fn_(item, env);

            if (result.has_value()) {
                co_return result;
            }

            last_result = std::move(result);
        }

        // 최대 시도 횟수 초과 — DLQ로 전송
        if (dlq_) {
            dlq_->push(std::move(item), env.ctx, last_result.error(), max_attempts_);
        }

        co_return last_result;
    }

private:
    InnerFn                              fn_;
    std::shared_ptr<DeadLetterQueue<In>> dlq_;
    size_t                               max_attempts_;
};

} // namespace qbuem

/** @} */
