#pragma once

#include <qbuem/core/reactor.hpp>

#include <functional>
#include <span>
#include <sys/uio.h>
#include <unordered_map>

// Forward-declare the liburing type so consumers don't need liburing.h
struct io_uring;

namespace qbuem {

/**
 * @brief Linux io_uring Reactor.
 *
 * Uses io_uring's POLL_ADD operation for I/O readiness notification and
 * IORING_OP_TIMEOUT for timer support. Both are naturally one-shot; the
 * reactor re-submits them automatically to maintain persistent-event semantics.
 *
 * The complete io_uring ring is owned internally and the full liburing API is
 * only needed in the .cpp translation unit.
 */
class IOUringReactor final : public Reactor {
public:
  static constexpr unsigned QUEUE_DEPTH = 256;

  /**
   * @brief SQPOLL 모드 활성화 여부를 반환합니다.
   *
   * 커널이 IORING_SETUP_SQPOLL을 수락하면 true.
   * 권한 부족 등으로 일반 모드로 폴백된 경우 false.
   */
  bool is_sqpoll() const noexcept;

  IOUringReactor();
  ~IOUringReactor() override;

  Result<void> register_event(int fd, EventType type,
                              std::function<void(int)> callback) override;

  Result<int> register_timer(int timeout_ms,
                             std::function<void(int)> callback) override;

  Result<void> unregister_event(int fd, EventType type) override;

  Result<void> unregister_timer(int timer_id) override;

  Result<int> poll(int timeout_ms) override;

  void stop() override;
  bool is_running() const override;

  void post(std::function<void()> fn) override;

  // -------------------------------------------------------------------------
  // Fixed Buffer API  (io_uring_register_buffers — DMA 직접 쓰기)
  // -------------------------------------------------------------------------

  /**
   * @brief `iovec` 배열을 커널에 고정 버퍼로 등록합니다.
   *
   * 등록된 버퍼는 페이지 테이블에 고정(pinned)되어 DMA 연산 시
   * 복사 없이 직접 사용 가능합니다. `read_fixed()` / `write_fixed()`
   * 호출 시 버퍼 인덱스로 참조합니다.
   *
   * @param iovecs 등록할 iovec 배열. 수명은 `unregister_fixed_buffers()`
   *               호출 시까지 유지해야 합니다.
   * @returns `ok()` 또는 에러 코드.
   *
   * @note 이미 등록된 경우 먼저 `unregister_fixed_buffers()`를 호출하세요.
   */
  Result<void> register_fixed_buffers(std::span<const iovec> iovecs);

  /**
   * @brief 등록된 고정 버퍼를 모두 해제합니다.
   */
  void unregister_fixed_buffers() noexcept;

  /**
   * @brief 현재 등록된 고정 버퍼 수를 반환합니다.
   */
  size_t fixed_buffer_count() const noexcept;

  /**
   * @brief `IORING_OP_READ_FIXED` — 고정 버퍼로 비동기 읽기.
   *
   * 일반 read()와 달리 커널이 버퍼를 이미 알고 있어 복사가 없습니다.
   *
   * @param fd        읽을 파일 디스크립터.
   * @param buf_idx   `register_fixed_buffers()` 시 지정한 버퍼 인덱스.
   * @param buf       버퍼 인덱스 내 슬라이스 (주소 + 길이가 등록 범위 내여야 함).
   * @param file_offset 스트리밍 fd이면 -1; 파일이면 실제 오프셋.
   * @param callback  완료 시 호출. 양수=읽은 바이트, 음수=errno.
   */
  Result<void> read_fixed(int fd, int buf_idx, std::span<std::byte> buf,
                          int64_t file_offset,
                          std::function<void(int)> callback);

  /**
   * @brief `IORING_OP_WRITE_FIXED` — 고정 버퍼로 비동기 쓰기.
   *
   * @param fd        쓸 파일 디스크립터.
   * @param buf_idx   등록된 버퍼 인덱스.
   * @param buf       버퍼 슬라이스.
   * @param file_offset 스트리밍 fd이면 -1.
   * @param callback  완료 시 호출. 양수=쓴 바이트, 음수=errno.
   */
  Result<void> write_fixed(int fd, int buf_idx, std::span<const std::byte> buf,
                           int64_t file_offset,
                           std::function<void(int)> callback);

  // -------------------------------------------------------------------------
  // Buffer Ring API  (IORING_OP_PROVIDE_BUFFERS — 커널 버퍼 자동 선택)
  // -------------------------------------------------------------------------

  /**
   * @brief 커널이 recv 시 버퍼를 자동 선택하는 Buffer Ring을 등록합니다.
   *
   * `IORING_OP_PROVIDE_BUFFERS`로 버퍼 풀을 커널에 전달합니다.
   * `recv_buffered()` 호출 시 커널이 적절한 버퍼를 선택하고
   * CQE flags에 선택된 버퍼 ID를 포함합니다.
   *
   * @param bgid         버퍼 그룹 ID (0~65535). 같은 ring에서 유일해야 함.
   * @param buf_size     개별 버퍼 크기 (바이트).
   * @param buf_count    버퍼 개수. 2의 거듭제곱 권장.
   * @returns `ok()` 또는 에러 코드.
   *
   * @note Linux 5.19+ / liburing 2.2+ 필요.
   */
  Result<void> register_buf_ring(uint16_t bgid, size_t buf_size,
                                 size_t buf_count);

  /**
   * @brief Buffer Ring을 해제합니다.
   *
   * @param bgid `register_buf_ring()`에서 지정한 그룹 ID.
   */
  void unregister_buf_ring(uint16_t bgid) noexcept;

  /**
   * @brief Buffer Ring을 사용한 비동기 recv.
   *
   * 커널이 `bgid` 그룹에서 빈 버퍼를 자동 선택하여 데이터를 채웁니다.
   * 완료 시 `callback(bytes_received, buf_id, buf_ptr)` 형태로 호출됩니다.
   * 콜백 내에서 버퍼를 소비한 후 `return_buf_to_ring(bgid, buf_id)` 필수.
   *
   * @param fd       recv할 소켓.
   * @param bgid     버퍼 그룹 ID.
   * @param callback 완료 콜백. bytes<0 이면 에러, buf_ptr=nullptr.
   */
  Result<void> recv_buffered(int fd, uint16_t bgid,
                             std::function<void(int, uint16_t, void *)> callback);

  /**
   * @brief 사용 완료된 버퍼를 Buffer Ring에 반환합니다.
   *
   * `recv_buffered()` 콜백 내에서 버퍼 처리 후 반드시 호출해야 합니다.
   * 반환하지 않으면 해당 버퍼가 재사용되지 않아 풀이 고갈됩니다.
   *
   * @param bgid   버퍼 그룹 ID.
   * @param buf_id 반환할 버퍼 ID (recv 콜백에서 전달됨).
   */
  void return_buf_to_ring(uint16_t bgid, uint16_t buf_id) noexcept;

private:
  // Pimpl: avoids exposing <liburing.h> in the public header.
  struct Impl;
  Impl *impl_;
};

} // namespace qbuem


} // namespace qbuem
