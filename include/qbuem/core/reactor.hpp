#pragma once

/**
 * @file qbuem/core/reactor.hpp
 * @brief 플랫폼 독립적인 이벤트 루프 Reactor 추상 인터페이스 정의.
 * @ingroup qbuem_io
 *
 * 이 헤더는 qbuem-stack의 I/O 이벤트 루프 핵심 추상화를 제공합니다.
 *
 * ### Shared-Nothing 설계
 * 각 `Reactor` 인스턴스는 **정확히 하나의 스레드**에서만 실행됩니다.
 * 스레드 간 데이터 공유를 최소화하여 락 경합과 캐시 무효화를 방지합니다.
 * 이 패턴은 Nginx, Redis, Node.js 등 고성능 서버에서 검증된 접근법입니다.
 *
 * ### 플랫폼 추상화
 * 구체 구현은 플랫폼에 따라 달라질 수 있습니다:
 * - Linux: `epoll` 기반 구현
 * - macOS/BSD: `kqueue` 기반 구현
 * - 테스트: 모의(mock) 구현
 *
 * ### 스레드 로컬 접근
 * 현재 스레드의 Reactor는 `Reactor::current()`로 접근합니다.
 * 이를 통해 코루틴 awaiters가 명시적 참조 전달 없이 Reactor에 접근할 수 있습니다.
 */

/**
 * @defgroup qbuem_io I/O & Event Loop
 * @brief Reactor, Dispatcher, Connection 등 I/O 이벤트 처리 컴포넌트.
 *
 * 이 그룹은 비동기 I/O의 핵심 인프라를 구성합니다:
 * - `Reactor`: 단일 스레드 이벤트 루프 추상화
 * - `Dispatcher`: 다중 Reactor를 관리하는 조율자
 * - `Connection`: 클라이언트 연결 수명 관리
 * @{
 */

#include <qbuem/common.hpp>
#include <functional>

namespace qbuem {

/**
 * @brief 파일 디스크립터 또는 타이머 이벤트의 종류를 나타내는 열거형.
 *
 * Reactor에 이벤트를 등록할 때 어떤 종류의 이벤트를 감시할지 지정합니다.
 *
 * - `Read`: 파일 디스크립터에서 데이터를 읽을 수 있을 때 (EPOLLIN/EVFILT_READ)
 * - `Write`: 파일 디스크립터에 데이터를 쓸 수 있을 때 (EPOLLOUT/EVFILT_WRITE)
 * - `Error`: 파일 디스크립터에 오류가 발생했을 때 (EPOLLERR/EVFILT_EXCEPT)
 */
enum class EventType { Read, Write, Error };

/**
 * @brief 플랫폼 독립적인 이벤트 루프 추상 인터페이스.
 *
 * `Reactor`는 파일 디스크립터와 타이머에 대한 비동기 이벤트를 처리합니다.
 * 각 인스턴스는 단일 스레드에서 전용으로 실행됩니다 (Shared-Nothing).
 *
 * 구체 클래스는 이 인터페이스를 플랫폼별로 구현해야 합니다.
 * 코루틴 기반 awaiters(`AsyncRead`, `AsyncWrite` 등)는 이 인터페이스를 통해
 * Reactor와 상호작용합니다.
 *
 * @note 이 클래스는 추상 클래스로, 직접 인스턴스화할 수 없습니다.
 *       플랫폼별 구체 구현을 사용하거나 테스트를 위해 모의 구현을 만드세요.
 *
 * ### 일반적인 사용 패턴
 * @code
 * // Reactor는 보통 Dispatcher가 생성하고 관리합니다.
 * // 코루틴 내에서는 current()로 현재 스레드의 Reactor에 접근합니다:
 * auto *reactor = qbuem::Reactor::current();
 * reactor->register_event(fd, EventType::Read, [](int f) {
 *     // fd가 읽기 가능해졌을 때 호출됩니다
 * });
 * @endcode
 */
class Reactor {
public:
  /** @brief 가상 소멸자. 파생 클래스가 자원을 안전하게 해제할 수 있도록 합니다. */
  virtual ~Reactor() = default;

  /**
   * @brief 파일 디스크립터를 Reactor에 등록하고 이벤트 콜백을 설정합니다.
   *
   * 지정한 이벤트가 발생하면 `callback`이 호출됩니다.
   * 동일한 fd와 type 조합을 중복 등록하면 기존 콜백을 덮어씁니다.
   *
   * @param fd       감시할 파일 디스크립터.
   * @param type     감시할 이벤트 종류 (`EventType::Read`, `Write`, `Error`).
   * @param callback 이벤트 발생 시 호출될 콜백. 인자로 fd를 전달받습니다.
   * @returns 성공 시 `Result<void>::ok()`, 실패 시 에러 코드.
   *
   * @note 이 함수는 Reactor가 실행 중인 스레드에서만 호출해야 합니다.
   * @warning fd는 반드시 `O_NONBLOCK` 플래그가 설정된 상태여야 합니다.
   *          블로킹 fd를 등록하면 이벤트 루프 전체가 블로킹될 수 있습니다.
   */
  virtual Result<void> register_event(int fd, EventType type,
                                      std::function<void(int)> callback) = 0;

  /**
   * @brief 타이머를 등록하고, 지정한 시간 후 콜백을 호출합니다.
   *
   * 타이머는 일회성입니다. 반복 타이머가 필요하면 콜백 내에서 다시 등록하세요.
   *
   * @param timeout_ms 타이머 만료까지의 대기 시간 (밀리초).
   * @param callback   타이머 만료 시 호출될 콜백. 인자로 timer ID를 전달받습니다.
   * @returns 성공 시 타이머 ID를 담은 `Result<int>`, 실패 시 에러 코드.
   *          반환된 ID는 `unregister_timer()`에서 타이머를 취소할 때 사용합니다.
   *
   * @note timeout_ms가 0이면 즉시 이벤트 루프에서 콜백을 호출합니다.
   */
  virtual Result<int> register_timer(int timeout_ms,
                                     std::function<void(int)> callback) = 0;

  /**
   * @brief 파일 디스크립터에 대한 이벤트 감시를 중단합니다.
   *
   * 코루틴 awaiters는 이벤트를 처리한 후 반드시 이 함수를 호출하여
   * 다음 이벤트 사이클에서 중복 처리되지 않도록 해야 합니다.
   *
   * @param fd   감시를 중단할 파일 디스크립터.
   * @param type 중단할 이벤트 종류.
   * @returns 성공 시 `Result<void>::ok()`, 실패 시 에러 코드.
   */
  virtual Result<void> unregister_event(int fd, EventType type) = 0;

  /**
   * @brief 등록된 타이머를 취소합니다.
   *
   * 아직 만료되지 않은 타이머를 취소할 때 사용합니다.
   * 이미 만료된 타이머 ID를 전달하면 무시됩니다.
   *
   * @param timer_id `register_timer()`에서 반환된 타이머 ID.
   * @returns 성공 시 `Result<void>::ok()`, 실패 시 에러 코드.
   */
  virtual Result<void> unregister_timer(int timer_id) = 0;

  /**
   * @brief Reactor의 이벤트 루프가 현재 실행 중인지 확인합니다.
   * @returns 실행 중이면 true, 그렇지 않으면 false.
   */
  virtual bool is_running() const = 0;

  /**
   * @brief 이벤트 루프를 단일 반복 실행합니다.
   *
   * 등록된 이벤트를 처리하고 반환합니다. `timeout_ms` 동안 이벤트가
   * 없으면 타임아웃 후 반환합니다.
   *
   * 이 함수는 Dispatcher의 워커 스레드 루프에서 반복 호출됩니다.
   *
   * @param timeout_ms 이벤트 대기 최대 시간 (밀리초). -1이면 이벤트가 발생할 때까지 무한 대기.
   * @returns 처리된 이벤트 수를 담은 `Result<int>`, 실패 시 에러 코드.
   */
  virtual Result<int> poll(int timeout_ms) = 0;

  /**
   * @brief 이벤트 루프를 정지시킵니다.
   *
   * 이 함수 호출 후 `is_running()`은 false를 반환하고,
   * 진행 중인 `poll()` 호출은 조기 반환합니다.
   *
   * @note 이 함수는 스레드 안전하게 구현되어야 합니다.
   *       다른 스레드에서 호출되어 현재 Reactor를 정지시킬 수 있어야 합니다.
   */
  virtual void stop() = 0;

  /**
   * @brief 현재 스레드에 연결된 Reactor를 반환합니다.
   *
   * 스레드 로컬 저장소(thread-local storage)를 통해 각 스레드에 고유한
   * Reactor 인스턴스를 반환합니다.
   *
   * 코루틴 awaiters는 이 함수를 통해 명시적 참조 전달 없이
   * 현재 스레드의 Reactor에 접근합니다.
   *
   * @returns 현재 스레드의 Reactor 포인터. `set_current()`를 호출하지 않았다면 nullptr.
   * @note Dispatcher가 워커 스레드를 시작할 때 `set_current()`를 호출합니다.
   */
  static Reactor *current();

  /**
   * @brief 현재 스레드의 Reactor를 설정합니다.
   *
   * Dispatcher가 각 워커 스레드 시작 시 호출합니다.
   * 이 함수 이후로 해당 스레드에서 `current()`를 호출하면 `r`을 반환합니다.
   *
   * @param r 현재 스레드에 연결할 Reactor 포인터. nullptr도 허용됩니다.
   */
  static void set_current(Reactor *r);
};

} // namespace qbuem

/** @} */ // end of qbuem_io
