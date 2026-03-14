#pragma once

/**
 * @file qbuem/codec/http1_codec.hpp
 * @brief HTTP/1.1 요청 코덱 — IFrameCodec<http::Request> 래퍼
 * @ingroup qbuem_codec
 *
 * `HttpParser`를 `IFrameCodec<Request>` 인터페이스로 감싸는 어댑터입니다.
 *
 * ### 동작 개요
 * - `decode()`: `HttpParser`를 사용하여 HTTP/1.1 요청을 점진적으로 파싱합니다.
 * - `encode()`: HTTP 서버는 요청을 인코딩하지 않습니다.
 *              응답 직렬화는 `http::Response`와 `http::Response::serialize()`를 사용하세요.
 * - `reset()` : 파서를 재생성하여 다음 요청 파싱 준비를 합니다.
 *
 * ### 사용 예시
 * @code
 * Http1Codec codec;
 * Request req;
 * auto status = codec.decode(recv_buf, req);
 * if (status == DecodeStatus::Complete) {
 *   handle_request(req);
 *   codec.reset(); // keep-alive: 다음 요청 파싱 준비
 * } else if (status == DecodeStatus::Error) {
 *   send_400_bad_request();
 * }
 * @endcode
 *
 * @{
 */

#include <qbuem/codec/frame_codec.hpp>
#include <qbuem/http/parser.hpp>
#include <qbuem/http/request.hpp>

#include <memory_resource>
#include <string_view>

namespace qbuem::codec {

/**
 * @brief HTTP/1.1 요청 파서를 `IFrameCodec<Request>` 인터페이스로 감싸는 코덱.
 *
 * 내부적으로 `HttpParser` 상태 머신을 유지합니다.
 * `reset()`을 호출하면 파서가 재초기화되어 다음 HTTP 요청을 파싱할 수 있습니다.
 * HTTP keep-alive 연결에서 연속적인 요청 처리에 적합합니다.
 *
 * ### encode() 주의사항
 * HTTP 서버 코덱에서 `encode(Request)`는 의미가 없습니다.
 * 항상 `DecodeStatus::Error`에 해당하는 0을 반환합니다.
 * HTTP 응답 직렬화는 `http::Response`를 사용하세요.
 */
class Http1Codec : public IFrameCodec<Request> {
public:
  /** @brief 기본 생성자. HttpParser를 초기화합니다. */
  Http1Codec() = default;

  /**
   * @brief 버퍼에서 HTTP/1.1 요청을 디코딩합니다.
   *
   * 내부 `HttpParser`에 데이터를 공급하여 요청을 점진적으로 파싱합니다.
   * 완성된 요청은 `out`에 저장됩니다.
   *
   * 성공 시 `buf`는 소비된 바이트만큼 앞으로 이동합니다.
   * 다음 요청 파싱을 위해 반드시 `reset()`을 호출하세요.
   *
   * @param[in,out] buf 원시 바이트 뷰. `Complete` 시 소비 바이트만큼 이동.
   * @param[out]    out 디코딩된 HTTP 요청. `Complete` 시에만 유효.
   * @returns `Complete` = 요청 완성, `Incomplete` = 데이터 부족,
   *          `Error` = HTTP 파싱 오류 (400/413 등).
   */
  DecodeStatus decode(BufferView &buf, Request &out) override {
    if (buf.empty()) return DecodeStatus::Incomplete;

    std::string_view sv(reinterpret_cast<const char *>(buf.data()), buf.size());
    auto consumed = parser_.parse(sv, out);

    if (!consumed.has_value()) {
      return DecodeStatus::Error;
    }

    if (!parser_.is_complete()) {
      // 모든 바이트를 소비하되 완성되지 않음
      buf = buf.subspan(buf.size());
      return DecodeStatus::Incomplete;
    }

    buf = buf.subspan(*consumed);
    return DecodeStatus::Complete;
  }

  /**
   * @brief HTTP 요청 인코딩 — 지원하지 않습니다.
   *
   * HTTP 서버는 요청을 인코딩하지 않습니다.
   * 응답 인코딩은 `http::Response::serialize()`를 사용하세요.
   *
   * @returns 항상 0 (실패).
   */
  size_t encode(const Request & /*frame*/, iovec * /*vecs*/, size_t /*max_vecs*/,
                std::pmr::memory_resource * /*arena*/) override {
    return 0;
  }

  /**
   * @brief HTTP 파서를 재초기화합니다.
   *
   * keep-alive 연결에서 이전 요청 처리 후 다음 요청 파싱을 위해 호출합니다.
   * 에러 복구 시에도 호출합니다.
   */
  void reset() override {
    parser_ = HttpParser{};
  }

  /**
   * @brief HTTP 헤더 파싱 완료 여부를 반환합니다.
   *
   * `Expect: 100-continue` 요청 처리 시 헤더 완료 시점에
   * `100 Continue` 응답을 전송해야 할 때 사용합니다.
   *
   * @returns 헤더 섹션이 완전히 파싱됐으면 true.
   */
  [[nodiscard]] bool headers_complete() const noexcept {
    return parser_.headers_complete();
  }

  /**
   * @brief 파싱 오류에 대응하는 HTTP 상태 코드를 반환합니다.
   *
   * - 400: 잘못된 HTTP 요청 구문
   * - 413: 페이로드가 허용 크기 초과
   *
   * @returns HTTP 에러 상태 코드 (400 또는 413).
   */
  [[nodiscard]] int error_status() const noexcept {
    return parser_.error_status();
  }

private:
  /** @brief HTTP/1.1 요청 파서 상태 머신. */
  HttpParser parser_;
};

} // namespace qbuem::codec

/** @} */ // end of qbuem_codec (http1)
