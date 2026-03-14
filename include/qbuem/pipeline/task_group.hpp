#pragma once

/**
 * @file qbuem/pipeline/task_group.hpp
 * @brief 구조적 동시성 — TaskGroup (Nursery 패턴)
 * @defgroup qbuem_task_group TaskGroup
 * @ingroup qbuem_pipeline
 *
 * TaskGroup은 자식 코루틴들의 수명을 부모에 묶는 구조적 동시성 프리미티브입니다.
 *
 * ## 보장사항
 * - 부모 코루틴이 `join()` / `join_all<T>()` 완료 전에 반환하지 않습니다.
 * - 자식 하나가 실패하면 나머지 자식에 취소 신호를 보내고 에러를 전파합니다.
 * - `join()` 완료 후 모든 자식 코루틴 프레임이 소멸됩니다.
 *
 * ## 사용 예시
 * @code
 * TaskGroup tg;
 * tg.spawn(task_a(env));
 * tg.spawn(task_b(env));
 * co_await tg.join();  // 모두 완료 대기
 *
 * // 결과 수집:
 * TaskGroup tg2;
 * tg2.spawn<int>(compute_a());
 * tg2.spawn<int>(compute_b());
 * auto res = co_await tg2.join_all<int>(); // Result<vector<int>>
 * @endcode
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>

#include <any>
#include <atomic>
#include <coroutine>
#include <memory>
#include <mutex>
#include <stop_token>
#include <vector>

namespace qbuem {

/**
 * @brief 구조적 동시성 — 자식 코루틴 그룹 관리.
 *
 * TaskGroup은 복사 불가입니다.
 * 스택 또는 힙에 생성하고 `spawn()` 후 `join()`/`join_all()`을 `co_await`합니다.
 */
class TaskGroup {
public:
  TaskGroup() : state_(std::make_shared<State>()) {}
  ~TaskGroup() = default;

  TaskGroup(const TaskGroup &) = delete;
  TaskGroup &operator=(const TaskGroup &) = delete;
  TaskGroup(TaskGroup &&) = default;
  TaskGroup &operator=(TaskGroup &&) = default;

  // -------------------------------------------------------------------------
  // spawn overloads
  // -------------------------------------------------------------------------

  /**
   * @brief void 코루틴 Task를 그룹에 추가합니다.
   *
   * Task는 현재 Reactor에서 post()를 통해 실행됩니다.
   *
   * @param task 실행할 Task<Result<void>>. 소유권이 이전됩니다.
   */
  void spawn(Task<Result<void>> task) {
    state_->pending.fetch_add(1, std::memory_order_relaxed);
    launch(run_void(std::move(task), state_));
  }

  /**
   * @brief 결과를 반환하는 코루틴 Task를 그룹에 추가합니다.
   *
   * @tparam T 결과 타입.
   * @param task 실행할 Task<Result<T>>.
   */
  template <typename T>
  void spawn(Task<Result<T>> task) {
    state_->pending.fetch_add(1, std::memory_order_relaxed);
    launch(run_typed<T>(std::move(task), state_));
  }

  // -------------------------------------------------------------------------
  // join
  // -------------------------------------------------------------------------

  /**
   * @brief 모든 자식 코루틴이 완료될 때까지 대기합니다.
   *
   * 자식 중 하나라도 실패하면 나머지를 취소하고 첫 에러를 반환합니다.
   *
   * @returns 성공 시 `Result<void>{}`, 실패 시 첫 에러 코드.
   */
  Task<Result<void>> join() {
    co_return co_await JoinAwaiter{state_, Reactor::current()};
  }

  /**
   * @brief 모든 자식의 결과를 수집합니다.
   *
   * @tparam T 결과 타입 (spawn<T>() 호출과 일치해야 함).
   * @returns `Result<std::vector<T>>`.
   */
  template <typename T>
  Task<Result<std::vector<T>>> join_all() {
    auto r = co_await JoinAwaiter{state_, Reactor::current()};
    if (!r.has_value())
      co_return unexpected(r.error());

    std::vector<T> results;
    std::lock_guard lock(state_->results_mutex);
    results.reserve(state_->results.size());
    for (auto &a : state_->results)
      results.push_back(std::any_cast<T>(std::move(a)));
    co_return results;
  }

  /**
   * @brief 모든 자식에 취소 신호를 보냅니다.
   */
  void cancel() { state_->stop_src.request_stop(); }

  /**
   * @brief 자식 취소 신호 토큰을 반환합니다.
   */
  [[nodiscard]] std::stop_token stop_token() const noexcept {
    return state_->stop_src.get_token();
  }

private:
  // -------------------------------------------------------------------------
  // Shared state between TaskGroup and all child wrappers
  // -------------------------------------------------------------------------
  struct State {
    std::atomic<size_t>      pending{0};
    std::atomic<bool>        has_error{false};
    std::error_code          first_error;
    std::mutex               error_mutex;

    std::stop_source         stop_src;

    // Typed results (join_all)
    std::mutex               results_mutex;
    std::vector<std::any>    results;

    // join() awaiter
    std::mutex               waiters_mutex;
    std::coroutine_handle<>  join_waiter;
    Reactor                 *join_reactor = nullptr;

    void notify_done(std::error_code ec) {
      if (ec && !has_error.exchange(true, std::memory_order_relaxed)) {
        std::lock_guard lock(error_mutex);
        first_error = ec;
        stop_src.request_stop();
      }
      size_t rem = pending.fetch_sub(1, std::memory_order_acq_rel) - 1;
      if (rem == 0)
        wake_join_waiter();
    }

    void wake_join_waiter() {
      std::coroutine_handle<> waiter;
      Reactor *r = nullptr;
      {
        std::lock_guard lock(waiters_mutex);
        waiter = join_waiter;
        r      = join_reactor;
        join_waiter  = {};
        join_reactor = nullptr;
      }
      if (!waiter)
        return;
      if (r)
        r->post([waiter]() mutable { waiter.resume(); });
      else
        waiter.resume();
    }
  };

  std::shared_ptr<State> state_;

  // -------------------------------------------------------------------------
  // Launch a wrapper coroutine via Reactor::post()
  // -------------------------------------------------------------------------
  static void launch(Task<void> wrapper) {
    auto h = wrapper.handle;
    wrapper.detach();
    Reactor *r = Reactor::current();
    if (r)
      r->post([h]() mutable { h.resume(); });
    else
      h.resume();
  }

  // -------------------------------------------------------------------------
  // Wrapper coroutines
  // -------------------------------------------------------------------------

  static Task<void> run_void(Task<Result<void>> task,
                             std::shared_ptr<State> st) {
    auto result = co_await task;
    std::error_code ec;
    if (!result.has_value())
      ec = result.error();
    st->notify_done(ec);
    co_return;
  }

  template <typename T>
  static Task<void> run_typed(Task<Result<T>> task,
                              std::shared_ptr<State> st) {
    auto result = co_await task;
    if (result.has_value()) {
      std::lock_guard lock(st->results_mutex);
      st->results.emplace_back(*std::move(result));
    }
    std::error_code ec;
    if (!result.has_value())
      ec = result.error();
    st->notify_done(ec);
    co_return;
  }

  // -------------------------------------------------------------------------
  // join() awaiter
  // -------------------------------------------------------------------------
  struct JoinAwaiter {
    std::shared_ptr<State> state;
    Reactor               *reactor;

    bool await_ready() const noexcept {
      return state->pending.load(std::memory_order_acquire) == 0;
    }

    bool await_suspend(std::coroutine_handle<> h) {
      std::lock_guard lock(state->waiters_mutex);
      // Double-check: may have reached zero between await_ready and here
      if (state->pending.load(std::memory_order_acquire) == 0)
        return false; // don't suspend, resume immediately
      state->join_waiter  = h;
      state->join_reactor = reactor;
      return true;
    }

    Result<void> await_resume() const {
      if (state->has_error.load(std::memory_order_acquire))
        return unexpected(state->first_error);
      return Result<void>{};
    }
  };
};

} // namespace qbuem

/** @} */
