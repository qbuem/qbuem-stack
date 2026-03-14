#pragma once

/**
 * @file qbuem/core/session_store.hpp
 * @brief 세션 저장소 추상 인터페이스 — ISessionStore
 * @defgroup qbuem_session Session Store
 * @ingroup qbuem_core
 *
 * 세션 데이터의 저장/조회/만료를 추상화합니다.
 * Redis, 인메모리, DB 등의 구현체는 서비스에서 이 인터페이스를 구현합니다.
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace qbuem {

/**
 * @brief 세션 저장소 추상 인터페이스.
 *
 * 세션 ID → 직렬화된 세션 데이터(문자열) 매핑을 TTL과 함께 관리합니다.
 * 직렬화 형식(JSON, MessagePack 등)은 서비스에서 결정합니다.
 *
 * ### 스레드 안전성
 * 구현체는 여러 워커 스레드에서 동시 호출될 수 있으므로 스레드 안전해야 합니다.
 *
 * ### 사용 예시
 * @code
 * // Redis 구현체를 ServiceRegistry에 등록
 * global_registry().register_singleton<ISessionStore>(
 *     std::make_shared<RedisSessionStore>("redis://localhost:6379"));
 *
 * // 라우트 핸들러에서 사용
 * auto store = global_registry().require<ISessionStore>();
 * auto data = co_await store->get(req.cookie("session_id"));
 * @endcode
 */
class ISessionStore {
public:
  virtual ~ISessionStore() = default;

  /**
   * @brief 세션 데이터를 조회합니다.
   *
   * @param session_id 세션 식별자.
   * @returns 세션 데이터 문자열. 세션이 없거나 만료되면 `std::nullopt`.
   */
  virtual Task<Result<std::optional<std::string>>>
  get(std::string_view session_id) = 0;

  /**
   * @brief 세션 데이터를 저장(upsert)합니다.
   *
   * 이미 존재하는 세션이면 덮어쓰고 TTL을 갱신합니다.
   *
   * @param session_id 세션 식별자.
   * @param value      직렬화된 세션 데이터.
   * @param ttl        세션 유효 시간. 기본 1시간.
   */
  virtual Task<Result<void>>
  set(std::string_view session_id, std::string value,
      std::chrono::seconds ttl = std::chrono::seconds{3600}) = 0;

  /**
   * @brief 세션을 삭제합니다 (로그아웃 등).
   *
   * 없는 세션을 삭제해도 에러를 반환하지 않습니다.
   *
   * @param session_id 삭제할 세션 식별자.
   */
  virtual Task<Result<void>> del(std::string_view session_id) = 0;

  /**
   * @brief 세션 TTL을 연장합니다 (슬라이딩 만료).
   *
   * 세션 데이터를 변경하지 않고 만료 시간만 갱신합니다.
   * 세션이 이미 만료되었으면 에러를 반환합니다.
   *
   * @param session_id 갱신할 세션 식별자.
   * @param ttl        새로운 유효 시간. 기본 1시간.
   */
  virtual Task<Result<void>>
  touch(std::string_view session_id,
        std::chrono::seconds ttl = std::chrono::seconds{3600}) = 0;

  /**
   * @brief 세션 존재 여부를 확인합니다.
   *
   * `get()` 호출 없이 존재 여부만 확인할 때 사용합니다.
   * 기본 구현은 `get() != nullopt`로 동작합니다.
   */
  virtual Task<Result<bool>> exists(std::string_view session_id) {
    auto r = co_await get(session_id);
    if (!r.has_value())
      co_return unexpected(r.error());
    co_return r->has_value();
  }
};

} // namespace qbuem

/** @} */
