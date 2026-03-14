#pragma once

/**
 * @file qbuem/pipeline/concepts.hpp
 * @brief 파이프라인 타입 안전성을 위한 C++20 Concepts
 * @defgroup qbuem_pipeline_concepts Concepts
 * @ingroup qbuem_pipeline
 *
 * Action 함수 서명을 컴파일 타임에 검증합니다.
 * 잘못된 서명은 명확한 컴파일 에러로 표현됩니다.
 *
 * ## Action 함수 서명 형태
 * 1. **FullActionFn**: `Task<Result<Out>>(In, ActionEnv)` — 컨텍스트 + 취소 신호 포함
 * 2. **SimpleActionFn**: `Task<Result<Out>>(In, std::stop_token)` — 취소 신호만
 * 3. **PlainActionFn**: `Task<Result<Out>>(In)` — 최소 서명
 * 4. **BatchActionFn**: `Task<Result<void>>(std::span<In>, std::span<Out>, ActionEnv)`
 *
 * `ActionFn<Fn,In,Out>` = FullActionFn ∨ SimpleActionFn ∨ PlainActionFn
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action_env.hpp>

#include <concepts>
#include <span>
#include <stop_token>
#include <type_traits>

namespace qbuem {

// ---------------------------------------------------------------------------
// Action function concepts
// ---------------------------------------------------------------------------

/**
 * @brief Full Action 서명 검증: `Task<Result<Out>>(In, ActionEnv)`.
 *
 * 컨텍스트(TraceCtx, RequestId 등) + 취소 신호 + 워커 인덱스 모두 접근 가능.
 */
template <typename Fn, typename In, typename Out>
concept FullActionFn =
    requires(Fn fn, In in, ActionEnv env) {
      { fn(std::move(in), env) } -> std::same_as<Task<Result<Out>>>;
    };

/**
 * @brief Simple Action 서명 검증: `Task<Result<Out>>(In, std::stop_token)`.
 *
 * 취소 신호만 사용하고 컨텍스트가 필요 없는 경우.
 */
template <typename Fn, typename In, typename Out>
concept SimpleActionFn =
    requires(Fn fn, In in, std::stop_token stop) {
      { fn(std::move(in), stop) } -> std::same_as<Task<Result<Out>>>;
    };

/**
 * @brief Plain Action 서명 검증: `Task<Result<Out>>(In)`.
 *
 * 취소 신호도 컨텍스트도 불필요한 순수 변환 함수.
 */
template <typename Fn, typename In, typename Out>
concept PlainActionFn =
    requires(Fn fn, In in) {
      { fn(std::move(in)) } -> std::same_as<Task<Result<Out>>>;
    };

/**
 * @brief Action 함수 서명 검증 — 세 가지 형태 중 하나.
 *
 * `FullActionFn ∨ SimpleActionFn ∨ PlainActionFn`
 */
template <typename Fn, typename In, typename Out>
concept ActionFn = FullActionFn<Fn, In, Out> ||
                   SimpleActionFn<Fn, In, Out> ||
                   PlainActionFn<Fn, In, Out>;

/**
 * @brief Batch Action 서명 검증.
 *
 * 서명: `Task<Result<void>>(std::span<In>, std::span<Out>, ActionEnv)`
 *
 * `span<Out>` 크기는 `span<In>` 크기와 같거나 그 이상이어야 합니다.
 * DB bulk insert, 배치 ML 추론 등에 사용합니다.
 */
template <typename Fn, typename In, typename Out>
concept BatchActionFn =
    requires(Fn fn, std::span<In> in, std::span<Out> out, ActionEnv env) {
      { fn(in, out, env) } -> std::same_as<Task<Result<void>>>;
    };

// ---------------------------------------------------------------------------
// Pipeline input concept
// ---------------------------------------------------------------------------

/**
 * @brief 파이프라인 입력 인터페이스 검증.
 *
 * `push(In)` 메서드를 가지는 타입임을 보장합니다.
 * fan-out 연결 시 타입 안전성 확인에 사용됩니다.
 */
template <typename Pipeline, typename In>
concept PipelineInputFor =
    requires(Pipeline &p, In item) {
      { p.push(std::move(item)) } -> std::same_as<Task<Result<void>>>;
    };

// ---------------------------------------------------------------------------
// Adapter: ActionFn을 FullActionFn으로 래핑
// ---------------------------------------------------------------------------

/**
 * @brief `SimpleActionFn` 또는 `PlainActionFn`을 `FullActionFn`으로 변환합니다.
 *
 * Action 내부에서 통일된 서명으로 처리하기 위해 사용합니다.
 *
 * @tparam Fn  원본 함수 타입.
 * @tparam In  입력 타입.
 * @tparam Out 출력 타입.
 * @param  fn  원본 함수.
 * @returns `Task<Result<Out>>(In, ActionEnv)` 서명의 래퍼 람다.
 */
template <typename Fn, typename In, typename Out>
  requires ActionFn<Fn, In, Out>
auto to_full_action_fn(Fn fn) {
  if constexpr (FullActionFn<Fn, In, Out>) {
    return fn; // 이미 Full 서명 — 그대로 반환
  } else if constexpr (SimpleActionFn<Fn, In, Out>) {
    return [fn = std::move(fn)](In in, ActionEnv env) mutable
               -> Task<Result<Out>> {
      return fn(std::move(in), env.stop);
    };
  } else { // PlainActionFn
    return [fn = std::move(fn)](In in, ActionEnv) mutable
               -> Task<Result<Out>> {
      return fn(std::move(in));
    };
  }
}

} // namespace qbuem

/** @} */
