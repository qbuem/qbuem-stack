#pragma once

/**
 * @file qbuem/codec/frame_codec.hpp
 * @brief 프레임 코덱 추상 인터페이스 — IFrameCodec<Frame>
 * @defgroup qbuem_codec Frame Codecs
 * @ingroup qbuem_net
 *
 * 이 헤더는 프로토콜 프레이밍의 최상위 추상화를 정의합니다.
 *
 * - `DecodeStatus` : 디코딩 결과를 나타내는 열거형
 * - `IFrameCodec<Frame>` : 모든 코덱이 구현해야 할 순수 가상 인터페이스
 *
 * 구체 코덱 구현은 별도 헤더에 위치합니다:
 * - `<qbuem/codec/length_prefix_codec.hpp>` : 4바이트 길이 접두사 프레이밍
 * - `<qbuem/codec/line_codec.hpp>` : 줄 구분자 프레이밍 (RESP, SMTP 등)
 * - `<qbuem/codec/http1_codec.hpp>` : HTTP/1.1 요청 코덱
 *
 * ### 설계 원칙
 * - 제로 가상 오버헤드: 핫 패스에서 static dispatch를 위해 CRTP 패턴도 고려 가능
 * - 점진적 파싱: decode()는 불완전한 데이터에서 `Incomplete`를 반환
 * - arena 기반 인코딩: encode()에 `pmr::memory_resource`를 전달하여 무힙 인코딩 지원
 *
 * @{
 */

#include <qbuem/common.hpp>

#include <cstddef>
#include <memory_resource>
#include <sys/uio.h>

namespace qbuem::codec {

// ─── DecodeStatus ─────────────────────────────────────────────────────────────

/**
 * @brief 프레임 디코딩 결과 상태를 나타내는 열거형.
 *
 * `IFrameCodec::decode()`의 반환값으로 사용됩니다.
 *
 * - `Complete`   : 프레임 하나가 완전히 디코딩됨. `out`에 결과 저장.
 * - `Incomplete` : 데이터가 부족하여 프레임 완성 불가. 더 많은 데이터 필요.
 * - `Error`      : 프로토콜 위반 또는 파싱 오류.
 *
 * @code
 * LineCodec codec;
 * Line line;
 * auto status = codec.decode(buf, line);
 * switch (status) {
 *   case DecodeStatus::Complete:   handle(line); break;
 *   case DecodeStatus::Incomplete: recv_more(); break;
 *   case DecodeStatus::Error:      close_conn(); break;
 * }
 * @endcode
 */
enum class DecodeStatus {
  Complete,   ///< 프레임이 완전히 디코딩됨
  Incomplete, ///< 데이터 부족 — 추가 데이터 수신 후 재시도
  Error       ///< 프로토콜 오류 — 연결 종료 필요
};

// ─── IFrameCodec<Frame> ───────────────────────────────────────────────────────

/**
 * @brief 프레임 코덱 추상 인터페이스.
 *
 * 각 코덱은 원시 바이트 스트림을 프로토콜 프레임으로 변환(decode)하거나
 * 프레임을 iovec 배열로 직렬화(encode)합니다.
 *
 * 상태를 내부에 유지하므로 여러 번 decode()를 호출하여 점진적으로 데이터를 공급할 수 있습니다.
 * `reset()`으로 상태를 초기화하여 다음 프레임 파싱을 준비합니다.
 *
 * ### 사용 예시
 * @code
 * LengthPrefixedCodec codec;
 * LengthPrefixedFrame frame;
 * auto status = codec.decode(buf, frame);
 * if (status == DecodeStatus::Complete) {
 *   handle(frame);
 *   codec.reset();
 * }
 * @endcode
 *
 * @tparam Frame 코덱이 처리할 프레임 타입.
 */
template <typename Frame>
class IFrameCodec {
public:
  /** @brief 가상 소멸자. 파생 클래스의 자원을 안전하게 해제합니다. */
  virtual ~IFrameCodec() = default;

  /**
   * @brief 버퍼에서 프레임을 디코딩합니다.
   *
   * 성공 시(`Complete`) `buf`의 `data`/`size`를 소비된 바이트만큼 전진시킵니다.
   * 상태를 내부에 유지하므로 `Incomplete` 반환 후 더 많은 데이터와 함께 재호출이 가능합니다.
   *
   * @param[in,out] buf 디코딩할 원시 바이트 뷰. `Complete` 시 소비된 만큼 앞으로 이동.
   * @param[out]    out 디코딩된 프레임 출력. `Complete` 시에만 유효.
   * @returns `Complete` = 프레임 완성, `Incomplete` = 데이터 부족, `Error` = 파싱 오류.
   */
  virtual DecodeStatus decode(BufferView &buf, Frame &out) = 0;

  /**
   * @brief 프레임을 iovec 배열로 인코딩합니다 (scatter-gather 쓰기용).
   *
   * zero-copy scatter-gather 쓰기를 지원합니다.
   * 반환된 iovec 항목들은 `arena`가 살아있는 동안 유효합니다.
   *
   * @param frame    인코딩할 프레임.
   * @param vecs     출력 iovec 배열.
   * @param max_vecs iovec 배열의 최대 항목 수.
   * @param arena    헤더 직렬화 등 임시 메모리 할당에 사용할 메모리 리소스.
   *                 nullptr 전달 시 내부 정적 버퍼를 사용합니다.
   * @returns 사용된 iovec 항목 수. 0이면 인코딩 실패.
   */
  virtual size_t encode(const Frame &frame, iovec *vecs, size_t max_vecs,
                        std::pmr::memory_resource *arena) = 0;

  /**
   * @brief 디코더 내부 상태를 초기화합니다.
   *
   * 에러 발생 후 또는 프레임 처리 완료 후 다음 프레임 파싱을 위해 호출합니다.
   * encode() 상태는 영향을 받지 않습니다.
   */
  virtual void reset() = 0;
};

} // namespace qbuem::codec

/** @} */ // end of qbuem_codec
