#pragma once

/**
 * @file qbuem/io/write_buf.hpp
 * @brief 쓰기 코르크(cork) 버퍼 — 여러 청크를 단일 writev()로 플러시.
 * @ingroup qbuem_io_buffers
 *
 * `WriteBuf`는 여러 번의 `append()` 호출로 데이터를 모아 두었다가
 * `as_iovec()`으로 단일 `writev(2)` syscall에 전달합니다.
 * 이를 통해 syscall 횟수와 TCP 패킷 단편화를 최소화합니다.
 *
 * ### 설계 원칙
 * - 내부 버퍼는 `std::vector<std::byte>` (미래에 Arena로 교체 예정)
 * - `as_iovec()`은 `IOVec<16>`을 반환 — 최대 16-segment writev
 * - `clear()` 후 버퍼 용량은 유지됨 (재할당 없음)
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/io/iovec.hpp>

#include <cstddef>
#include <cstring>
#include <string_view>
#include <vector>

namespace qbuem {

/**
 * @brief 쓰기 코르크 버퍼 — 청크 누적 후 단일 writev() 플러시.
 *
 * HTTP 응답, 프로토콜 프레임 등 헤더 + 바디와 같이 여러 조각으로
 * 구성된 데이터를 하나의 시스템 콜로 전송할 때 사용합니다.
 *
 * ### 사용 예시
 * @code
 * WriteBuf wbuf;
 * wbuf.append(header_view);
 * wbuf.append("\r\n");
 * wbuf.append(body_view);
 *
 * auto vec = wbuf.as_iovec();
 * ::writev(fd, vec.as_span().data(), static_cast<int>(vec.as_span().size()));
 * wbuf.clear();
 * @endcode
 */
class WriteBuf {
public:
  /**
   * @brief 초기 용량을 지정하여 WriteBuf를 구성합니다.
   *
   * @param initial_cap 초기 내부 버퍼 용량 (바이트). 기본값 4096.
   */
  explicit WriteBuf(size_t initial_cap = 4096) {
    buf_.reserve(initial_cap);
  }

  // ─── 데이터 추가 ──────────────────────────────────────────────────────────

  /**
   * @brief BufferView의 데이터를 버퍼에 복사합니다.
   *
   * @param data 복사할 읽기 전용 바이트 뷰.
   */
  void append(BufferView data) {
    const auto *src = reinterpret_cast<const std::byte *>(data.data());
    buf_.insert(buf_.end(), src, src + data.size());
  }

  /**
   * @brief 문자열 뷰의 내용을 버퍼에 복사합니다.
   *
   * @param sv 복사할 문자열 뷰.
   */
  void append(std::string_view sv) {
    const auto *src = reinterpret_cast<const std::byte *>(sv.data());
    buf_.insert(buf_.end(), src, src + sv.size());
  }

  /**
   * @brief raw 포인터와 길이로 데이터를 버퍼에 복사합니다.
   *
   * @param data 복사 원본 포인터.
   * @param len  복사할 바이트 수.
   */
  void append(const void *data, size_t len) {
    const auto *src = static_cast<const std::byte *>(data);
    buf_.insert(buf_.end(), src, src + len);
  }

  // ─── writev 변환 ──────────────────────────────────────────────────────────

  /**
   * @brief 버퍼 전체를 단일 iovec으로 변환합니다.
   *
   * `WriteBuf`는 단일 연속 버퍼이므로 항상 엔트리가 1개인 `IOVec<16>`을 반환합니다.
   * 빈 버퍼에서는 비어 있는 IOVec이 반환됩니다.
   *
   * @returns `IOVec<16>` — 전체 버퍼를 가리키는 단일 iovec 항목.
   * @note 반환된 IOVec은 `buf_`의 수명에 의존합니다.
   *       `as_iovec()` 호출 후 `append()` 또는 `clear()`를 호출하면 무효화됩니다.
   */
  [[nodiscard]] IOVec<16> as_iovec() const noexcept {
    IOVec<16> vec;
    if (!buf_.empty())
      vec.push(buf_.data(), buf_.size());
    return vec;
  }

  // ─── 상태 ─────────────────────────────────────────────────────────────────

  /**
   * @brief 현재 버퍼에 쌓인 바이트 수를 반환합니다.
   *
   * @returns 내부 버퍼의 바이트 수.
   */
  [[nodiscard]] size_t size() const noexcept { return buf_.size(); }

  /**
   * @brief 버퍼가 비어 있는지 확인합니다.
   *
   * @returns `size() == 0`이면 true.
   */
  [[nodiscard]] bool empty() const noexcept { return buf_.empty(); }

  /**
   * @brief 버퍼 내용을 지웁니다.
   *
   * 내부 용량(capacity)은 유지되므로 재할당이 발생하지 않습니다.
   */
  void clear() noexcept { buf_.clear(); }

private:
  /**
   * @brief 내부 데이터 저장소.
   *
   * 추후 Arena 기반 할당기로 교체할 수 있도록 `std::vector`로 구현합니다.
   */
  std::vector<std::byte> buf_;
};

} // namespace qbuem

/** @} */ // end of qbuem_io_buffers
