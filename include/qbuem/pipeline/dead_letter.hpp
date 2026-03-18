#pragma once

/**
 * @file qbuem/pipeline/dead_letter.hpp
 * @brief Dead letter queue — DeadLetter, DeadLetterQueue, DlqAction
 * @defgroup qbuem_dead_letter DeadLetterQueue
 * @ingroup qbuem_pipeline
 *
 * Dead Letter Queue implementation that collects failed items for later reprocessing or auditing.
 *
 * ## Usage example
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
 * @brief Dead letter entry — failed item + error + context + attempt count.
 *
 * @tparam T Item type.
 */
template <typename T>
struct DeadLetter {
    T item;                                                               ///< Failed item
    Context ctx;                                                          ///< Item context
    std::error_code error;                                                ///< Failure error code
    size_t attempt_count = 1;                                             ///< Total number of attempts
    std::chrono::system_clock::time_point failed_at =
        std::chrono::system_clock::now();                                 ///< Time of failure
};

/**
 * @brief Dead letter queue — collects failed items and supports reprocessing.
 *
 * Thread-safe. Drops the oldest entry when max_size is exceeded.
 *
 * @tparam T Item type.
 */
template <typename T>
class DeadLetterQueue {
public:
    /**
     * @brief Dead letter queue configuration.
     */
    struct Config {
        size_t max_size   = 10000; ///< Maximum number of entries (oldest entry dropped when exceeded)
        bool   persist_log = false; ///< If true, each entry is written to AsyncLogger
    };

    /**
     * @brief Creates a DeadLetterQueue with the specified configuration.
     * @param cfg Configuration (defaults are usable as-is).
     */
    explicit DeadLetterQueue(Config cfg = {})
        : cfg_(std::move(cfg)) {}

    /**
     * @brief Adds a dead letter entry (thread-safe).
     *
     * Drops the oldest entry when max_size is exceeded.
     *
     * @param letter Dead letter entry to add.
     */
    void push(DeadLetter<T> letter) {
        std::lock_guard lock(mtx_);
        // Drop the oldest entry (front) when max_size is exceeded
        if (cfg_.max_size > 0 && queue_.size() >= cfg_.max_size) {
            queue_.pop_front();
        }
        queue_.push_back(std::move(letter));
    }

    /**
     * @brief Adds a dead letter entry from an item and error information (thread-safe).
     *
     * @param item     Failed item.
     * @param ctx      Item context.
     * @param err      Failure error code.
     * @param attempts Total number of attempts (default 1).
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
     * @brief Removes and returns all dead letters (by move). Empties the queue.
     *
     * @returns Vector of all dead letters.
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
     * @brief Inspects dead letters without consuming them.
     *
     * @param max_n Maximum number of entries to inspect (default 100).
     * @returns Vector of const pointers to dead letters.
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
     * @brief Reprocesses dead letters.
     *
     * Applies fn to each entry and removes successfully processed ones from the queue.
     * Failed entries remain in the queue.
     *
     * @param fn    Reprocessing function.
     * @param max_n Maximum number of entries to reprocess (default SIZE_MAX = all).
     * @returns Number of successfully reprocessed entries.
     */
    Task<size_t> reprocess(
        std::function<Task<Result<void>>(T, Context)> fn,
        size_t max_n = SIZE_MAX)
    {
        // Take a snapshot of current entries, then process
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
                // Re-insert failed entries
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
