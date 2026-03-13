#pragma once

/**
 * @file qbuem/core/dispatcher.hpp
 * @brief 멀티코어 이벤트 루프 관리자 — 코어당 하나의 Reactor를 소유
 * @defgroup qbuem_dispatcher Dispatcher
 * @ingroup qbuem_core
 *
 * Dispatcher는 std::thread::hardware_concurrency()개의 Reactor를 생성하여
 * 각 코어에서 독립적으로 이벤트를 처리. 수신 fd를 등록하면 라운드-로빈으로
 * 워커 Reactor에 분배.
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/reactor.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace qbuem {

/**
 * @brief 다중 Reactor 워커 스레드를 관리하는 조율자(orchestrator).
 *
 * Dispatcher는 qbuem-stack에서 동시성의 진입점입니다.
 * 애플리케이션은 보통 하나의 Dispatcher만 생성하며, 이것이 모든 I/O 처리를 담당합니다.
 *
 * ### 핵심 책임
 * 1. **스레드 풀 관리**: `thread_count`개의 워커 스레드를 생성하고 관리합니다.
 * 2. **Reactor 할당**: 각 워커 스레드에 독립적인 Reactor를 할당합니다.
 * 3. **리스너 등록**: 수신 fd를 특정 워커 Reactor에 등록합니다.
 * 4. **로드 분산**: fd 해시 기반으로 워커를 선택합니다.
 *
 * ### 사용 예시
 * @code
 * qbuem::Dispatcher dispatcher; // CPU 코어 수만큼 워커 생성
 *
 * // 리스닝 소켓을 등록 — 적절한 워커 Reactor에 할당됩니다
 * dispatcher.register_listener(listen_fd, [](int fd) {
 *     // 새 연결 수락 처리
 * });
 *
 * dispatcher.run(); // 블로킹: 모든 워커 스레드가 종료될 때까지 대기
 * @endcode
 *
 * @note Dispatcher는 스레드 안전합니다. `stop()`은 다른 스레드에서 호출할 수 있습니다.
 * @note Dispatcher 자체는 복사 및 이동이 불가능합니다.
 */
class Dispatcher {
public:
  /**
   * @brief 지정한 워커 스레드 수로 Dispatcher를 생성합니다.
   *
   * 생성 시 Reactor 인스턴스들을 만들지만, 워커 스레드는 `run()`을 호출해야
   * 시작됩니다. 이를 통해 생성과 시작을 분리하여 초기 설정(리스너 등록 등)이
   * 이벤트 루프 시작 전에 이루어질 수 있습니다.
   *
   * @param thread_count 워커 스레드 수. 기본값은 하드웨어 동시 실행 스레드 수
   *                     (`std::thread::hardware_concurrency()`).
   *                     0을 전달하면 적어도 1개의 스레드를 생성합니다.
   */
  explicit Dispatcher(
      size_t thread_count = std::thread::hardware_concurrency());

  /**
   * @brief 모든 워커 스레드를 시작하고 완료될 때까지 블로킹합니다.
   *
   * 각 워커 스레드는 자신의 Reactor에서 이벤트 루프를 실행합니다.
   * `stop()`이 호출되거나 모든 워커 스레드가 종료될 때까지 블로킹됩니다.
   *
   * @note 이 함수는 보통 `main()` 또는 서버 엔트리 포인트의 마지막에 호출됩니다.
   */
  void run();

  /**
   * @brief 모든 워커 스레드와 Reactor를 정지시킵니다.
   *
   * 각 Reactor의 `stop()`을 호출하여 이벤트 루프를 종료합니다.
   * 이 함수는 스레드 안전하며 다른 스레드에서도 호출할 수 있습니다.
   *
   * @note `stop()` 호출 후 `run()`은 워커 스레드들이 종료되면 반환됩니다.
   */
  void stop();

  /**
   * @brief 수신 fd를 워커 Reactor에 등록합니다.
   *
   * fd를 적절한 워커 Reactor에 할당하고, 해당 Reactor에 읽기 이벤트 콜백을
   * 등록합니다. 콜백은 fd에서 새 데이터나 연결이 생길 때마다 호출됩니다.
   *
   * 동일한 fd는 항상 같은 워커 Reactor에 할당됩니다 (fd % thread_count 기반).
   * 이를 통해 연결의 수명 동안 동일한 스레드에서 처리가 이루어집니다.
   *
   * @param fd       감시할 파일 디스크립터 (보통 리스닝 소켓).
   * @param callback fd에서 이벤트 발생 시 호출될 콜백.
   * @returns 성공 시 `Result<void>::ok()`, 실패 시 에러 코드.
   *
   * @note `run()` 호출 전에 등록해야 이벤트 루프 시작 시점부터 감시됩니다.
   */
  Result<void> register_listener(int fd, std::function<void(int)> callback);

  /**
   * @brief fd에 할당된 워커 Reactor를 반환합니다.
   *
   * `register_listener()`와 동일한 할당 알고리즘을 사용합니다.
   * 특정 연결에서 직접 Reactor 접근이 필요할 때 사용합니다.
   *
   * @param fd 워커를 조회할 파일 디스크립터.
   * @returns 해당 fd에 할당된 Reactor 포인터. 워커가 없으면 nullptr.
   */
  Reactor *get_worker_reactor(int fd);

private:
  /** @brief 실행 상태 플래그. `run()`과 `stop()` 사이의 동기화에 사용됩니다. */
  std::atomic<bool> running_{false};

  /**
   * @brief 워커 Reactor 인스턴스들의 목록.
   *
   * 인덱스 i의 Reactor는 인덱스 i의 워커 스레드가 소유합니다.
   * 수명은 Dispatcher와 동일합니다.
   */
  std::vector<std::unique_ptr<Reactor>> reactors_;
};

} // namespace qbuem

/** @} */
