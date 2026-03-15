#pragma once

/**
 * @file qbuem/codec/line_codec.hpp
 * @brief 줄 구분자 기반 프레임 코덱 — RESP, SMTP, POP3, IMAP 등
 * @ingroup qbuem_codec
 *
 * `\n` 또는 `\r\n`으로 구분된 텍스트 줄을 프레임으로 처리하는 코덱입니다.
 *
 * ### 주요 특징
 * - zero-copy: `Line::data`는 원본 수신 버퍼를 직접 참조 (`string_view`)
 * - CRLF 모드: Redis RESP, SMTP, HTTP/1.x 헤더 등
 * - LF 모드: 단순 텍스트 프로토콜
 *
 * ### 주의사항
 * `Line::data`는 `decode()` 호출 시 전달한 `buf`의 수명에 종속됩니다.
 * `buf`가 파괴되거나 덮어쓰여지면 `Line::data`가 댕글링 뷰가 됩니다.
 *
 * @code
 * LineCodec codec(true); // CRLF 모드
 * Line line;
 * auto status = codec.decode(recv_buf, line);
 * if (status == DecodeStatus::Complete) {
 *   // line.data는 recv_buf를 참조
 *   process(line.data);
 *   codec.reset();
 * }
 * @endcode
 *
 * @{
 */

#include <qbuem/codec/frame_codec.hpp>

#include <cstring>
#include <memory_resource>
#include <string_view>

namespace qbuem::codec {

// ─── Line ─────────────────────────────────────────────────────────────────────

/**
 * @brief 구분자가 제거된 단일 텍스트 줄을 나타내는 뷰 타입.
 *
 * `data`는 원본 버퍼를 zero-copy로 참조합니다.
 * 줄 끝의 `\n` 또는 `\r\n`은 포함되지 않습니다.
 *
 * @warning `data`가 참조하는 버퍼의 수명이 이 객체보다 길어야 합니다.
 */
struct Line {
  /** @brief 줄 내용을 참조하는 뷰 (구분자 제외). 원본 버퍼에 대한 zero-copy 참조. */
  std::string_view data;
};

// ─── LineCodec ────────────────────────────────────────────────────────────────

/**
 * @brief 줄 구분자(`\n` 또는 `\r\n`) 기반 프레임 코덱.
 *
 * `IFrameCodec<Line>`을 구현합니다.
 * `decode()`는 버퍼에서 구분자를 탐색하고 zero-copy `string_view`를 반환합니다.
 *
 * ### CRLF vs LF 모드
 * - `crlf=true`: `\r\n`을 구분자로 사용. Redis RESP, SMTP, HTTP 헤더에 적합.
 * - `crlf=false`: `\n`만 구분자로 사용. 단순 텍스트 스트림에 적합.
 *
 * ### encode() 동작
 * iovec[0] = 줄 내용, iovec[1] = 구분자 (`\n` 또는 `\r\n`).
 * 구분자는 정적 문자열 리터럴을 참조하므로 추가 할당이 없습니다.
 */
class LineCodec : public IFrameCodec<Line> {
public:
  /**
   * @brief LineCodec을 구성합니다.
   *
   * @param crlf true이면 `\r\n` 구분자 모드, false이면 `\n` 단독 모드.
   */
  explicit LineCodec(bool crlf = true) : crlf_(crlf) {}

  /**
   * @brief 버퍼에서 한 줄을 디코딩합니다 (zero-copy).
   *
   * `buf`에서 `\n`(LF 모드) 또는 `\r\n`(CRLF 모드)을 탐색합니다.
   * 발견 시 `out.data`를 구분자 이전의 내용을 참조하는 `string_view`로 설정합니다.
   * 성공 시 `buf`는 구분자를 포함한 소비 바이트만큼 앞으로 이동합니다.
   *
   * @param[in,out] buf 원시 바이트 뷰. `Complete` 시 소비 바이트만큼 이동.
   * @param[out]    out 디코딩된 줄 뷰. `buf`의 수명에 종속됩니다.
   * @returns `Complete` | `Incomplete`. `Error`는 반환하지 않습니다.
   */
  DecodeStatus decode(BufferView &buf, Line &out) override {
    const auto *data = reinterpret_cast<const char *>(buf.data());
    size_t       sz  = buf.size();

    // \n 위치 탐색
    const char *lf = static_cast<const char *>(std::memchr(data, '\n', sz));
    if (!lf) return DecodeStatus::Incomplete;

    size_t lf_pos = static_cast<size_t>(lf - data);

    // 줄 끝 결정 (CRLF 모드에서 \r 제거)
    size_t line_end = lf_pos;
    if (crlf_ && lf_pos > 0 && data[lf_pos - 1] == '\r') {
      --line_end;
    }

    out.data = std::string_view(data, line_end);

    // buf를 구분자 다음으로 전진
    buf = buf.subspan(lf_pos + 1);
    return DecodeStatus::Complete;
  }

  /**
   * @brief 줄을 iovec 배열로 인코딩합니다.
   *
   * iovec[0] = 줄 내용 (zero-copy, `line.data` 직접 참조)
   * iovec[1] = 구분자 (`\n` 또는 `\r\n`, 정적 버퍼 참조)
   *
   * `max_vecs`는 최소 2 이상이어야 합니다.
   * `arena`는 이 코덱에서 사용하지 않습니다 (추가 할당 불필요).
   *
   * @param line     인코딩할 줄 (구분자 미포함).
   * @param vecs     출력 iovec 배열.
   * @param max_vecs iovec 배열 크기 (최소 2 필요).
   * @param arena    미사용. nullptr 가능.
   * @returns 사용된 iovec 항목 수 (2). `max_vecs < 2`이면 0.
   */
  size_t encode(const Line &line, iovec *vecs, size_t max_vecs,
                std::pmr::memory_resource * /*arena*/) override {
    if (max_vecs < 2) return 0;

    vecs[0].iov_base = const_cast<char *>(line.data.data());
    vecs[0].iov_len  = line.data.size();

    // 구분자 — 정적 리터럴 버퍼 사용 (추가 할당 없음)
    if (crlf_) {
      static const char kCRLF[] = "\r\n";
      vecs[1].iov_base = const_cast<char *>(kCRLF);
      vecs[1].iov_len  = 2;
    } else {
      static const char kLF[] = "\n";
      vecs[1].iov_base = const_cast<char *>(kLF);
      vecs[1].iov_len  = 1;
    }

    return 2;
  }

  /**
   * @brief 코덱 상태를 초기화합니다.
   *
   * `LineCodec`은 외부 버퍼에 의존하는 zero-copy 설계이므로
   * 내부 상태가 없습니다. 이 함수는 인터페이스 호환성을 위해 존재합니다.
   */
  void reset() override {
    // 내부 파서 상태 없음 — zero-copy 설계
  }

private:
  /** @brief CRLF 모드 플래그. true이면 `\r\n`, false이면 `\n` 구분자 사용. */
  bool crlf_;
};

} // namespace qbuem::codec

/** @} */ // end of qbuem_codec (line)
