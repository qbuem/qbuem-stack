#pragma once

/**
 * @file qbuem/codec/frame_codec.hpp
 * @brief 프레임 코덱 인터페이스 및 구현 — 길이 접두사, 줄 구분, HTTP/1.1
 * @defgroup qbuem_codec Frame Codecs
 * @ingroup qbuem_net
 *
 * 이 헤더는 프로토콜 프레이밍 추상화를 제공합니다:
 *
 * - `IFrameCodec<Frame>` : 코덱 추상 인터페이스
 * - `LengthPrefixedCodec<Header>` : N바이트 길이 접두사 프레이밍
 * - `LineCodec` : `\n` 또는 `\r\n` 구분자 프레이밍 (RESP, SMTP 등)
 * - `Http1Codec` : HTTP/1.1 요청 파서 / 응답 인코더
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/http/parser.hpp>
#include <qbuem/http/request.hpp>
#include <qbuem/http/response.hpp>

#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <sys/uio.h>

namespace qbuem {

// ─── IFrameCodec<Frame> ───────────────────────────────────────────────────────

/**
 * @brief 프레임 코덱 추상 인터페이스.
 *
 * 각 코덱은 원시 바이트 스트림을 프로토콜 프레임으로 변환(decode)하거나
 * 프레임을 iovec 배열로 직렬화(encode)합니다.
 *
 * ### 사용 예시
 * @code
 * LineCodec codec;
 * std::string line;
 * int n = codec.decode(buf, line);
 * if (n > 0) {
 *   // 완성된 라인 처리
 * }
 * @endcode
 *
 * @tparam Frame 코덱이 처리할 프레임 타입.
 */
template <typename Frame>
class IFrameCodec {
public:
  virtual ~IFrameCodec() = default;

  /**
   * @brief 버퍼에서 프레임을 디코딩합니다.
   *
   * 상태를 내부에 유지하므로 여러 번 호출해 점진적으로 데이터를 공급할 수 있습니다.
   *
   * @param buf 디코딩할 원시 바이트 버퍼.
   * @param out 디코딩된 프레임 출력.
   * @returns 소비한 바이트 수 (>0 = 성공),
   *          0 = 데이터 부족 (계속 공급 필요),
   *          -1 = 파싱 오류.
   */
  virtual int decode(std::span<const std::byte> buf, Frame &out) = 0;

  /**
   * @brief 프레임을 iovec 배열로 인코딩합니다.
   *
   * scatter-gather 쓰기를 지원합니다 — 복사 없이 writev()에 직접 전달 가능.
   *
   * @param frame    인코딩할 프레임.
   * @param vecs     출력 iovec 배열.
   * @param max_vecs iovec 배열의 최대 크기.
   * @returns 인코딩된 총 바이트 수, 오류 시 에러 코드.
   */
  virtual Result<size_t> encode(const Frame &frame, iovec *vecs,
                                 size_t max_vecs) = 0;
};

// ─── LengthPrefixedCodec<Header> ─────────────────────────────────────────────

/**
 * @brief N바이트 길이 접두사 프레임 코덱.
 *
 * `Header` 타입에는 `length` 필드와 `header_size` 상수가 있어야 합니다:
 *
 * @code
 * struct MyHeader {
 *   uint32_t length;
 *   static constexpr size_t header_size = sizeof(MyHeader);
 * };
 * @endcode
 *
 * 프레임 형식: `[Header][payload (Header::length 바이트)]`
 *
 * @tparam Header 헤더 구조체 타입 (`length` 필드 + `header_size` 상수 필요).
 */
template <typename Header>
class LengthPrefixedCodec
    : public IFrameCodec<std::pair<Header, std::vector<std::byte>>> {
public:
  using Frame = std::pair<Header, std::vector<std::byte>>;

  /**
   * @brief 버퍼에서 길이 접두사 프레임을 디코딩합니다.
   *
   * @param buf 원시 바이트 버퍼.
   * @param out 디코딩된 (헤더, 페이로드) 쌍.
   * @returns 소비한 바이트 수, 0 = 불완전, -1 = 오류.
   */
  int decode(std::span<const std::byte> buf, Frame &out) override {
    constexpr size_t hdr_sz = Header::header_size;

    // 헤더 최소 크기 확인
    if (buf.size() < hdr_sz) return 0;

    Header hdr{};
    std::memcpy(&hdr, buf.data(), hdr_sz);
    size_t payload_len = static_cast<size_t>(hdr.length);

    // 전체 프레임 크기 확인
    size_t total = hdr_sz + payload_len;
    if (buf.size() < total) return 0;

    // 페이로드 복사
    out.first = hdr;
    out.second.assign(buf.data() + hdr_sz,
                      buf.data() + hdr_sz + payload_len);
    return static_cast<int>(total);
  }

  /**
   * @brief (헤더, 페이로드) 쌍을 iovec 배열로 인코딩합니다.
   *
   * iovec[0] = 헤더, iovec[1] = 페이로드 (max_vecs >= 2 필요).
   *
   * @param frame    인코딩할 프레임.
   * @param vecs     출력 iovec 배열.
   * @param max_vecs iovec 배열의 최대 크기.
   * @returns 인코딩된 총 바이트 수, 오류 시 에러 코드.
   */
  Result<size_t> encode(const Frame &frame, iovec *vecs,
                         size_t max_vecs) override {
    if (max_vecs < 2)
      return unexpected(std::make_error_code(std::errc::no_buffer_space));

    constexpr size_t hdr_sz = Header::header_size;
    // 헤더를 로컬 버퍼에 복사 (라이프타임 주의 — 호출자가 encode 호출 후 즉시 writev 해야 함)
    header_buf_ = frame.first;

    vecs[0].iov_base = &header_buf_;
    vecs[0].iov_len  = hdr_sz;
    vecs[1].iov_base = const_cast<std::byte *>(frame.second.data());
    vecs[1].iov_len  = frame.second.size();

    return hdr_sz + frame.second.size();
  }

private:
  /** @brief iovec가 참조하는 헤더 버퍼 (encode 호출 간 유효). */
  Header header_buf_{};
};

// ─── LineCodec ───────────────────────────────────────────────────────────────

/**
 * @brief 줄 구분자(`\n` 또는 `\r\n`) 기반 프레임 코덱.
 *
 * RESP(Redis), SMTP, POP3, IMAP 등 텍스트 기반 프로토콜에 사용합니다.
 * 부분 데이터를 내부 버퍼에 누적하므로 스트림을 점진적으로 공급해도 됩니다.
 *
 * @code
 * LineCodec codec(true); // CRLF 모드
 * std::string line;
 * int n = codec.decode(buf, line);
 * if (n > 0) {
 *   // line에 완성된 줄 (구분자 제외)
 * }
 * @endcode
 */
class LineCodec : public IFrameCodec<std::string> {
public:
  /**
   * @brief LineCodec을 구성합니다.
   *
   * @param crlf true이면 `\r\n` 구분자 모드, false이면 `\n` 모드.
   */
  explicit LineCodec(bool crlf = false) : crlf_(crlf) {}

  /**
   * @brief 버퍼에서 한 줄을 디코딩합니다.
   *
   * 내부 부분 버퍼에 데이터를 누적합니다.
   * 구분자를 찾으면 해당 줄을 `out`에 저장하고 소비한 바이트 수를 반환합니다.
   *
   * @param buf 원시 바이트 버퍼.
   * @param out 디코딩된 줄 (구분자 제외).
   * @returns 소비한 바이트 수 (>0 = 줄 완성), 0 = 불완전.
   */
  int decode(std::span<const std::byte> buf, std::string &out) override {
    // 원시 바이트를 char로 변환하여 누적
    for (auto b : buf) {
      char c = static_cast<char>(b);
      partial_ += c;
    }

    // 구분자 탐색
    std::string_view view = partial_;
    size_t pos = view.find('\n');
    if (pos == std::string_view::npos) return 0;

    // 줄 추출 (CRLF 모드에서 \r 제거)
    size_t line_end = pos;
    if (crlf_ && line_end > 0 && view[line_end - 1] == '\r')
      --line_end;
    out = std::string(view.substr(0, line_end));

    // 소비한 부분 제거
    size_t consumed = pos + 1; // \n 포함
    partial_.erase(0, consumed);
    return static_cast<int>(consumed);
  }

  /**
   * @brief 줄을 iovec 배열로 인코딩합니다.
   *
   * iovec[0] = 줄 내용, iovec[1] = 구분자 (`\n` 또는 `\r\n`).
   *
   * @param line     인코딩할 줄 (구분자 미포함).
   * @param vecs     출력 iovec 배열.
   * @param max_vecs iovec 배열의 최대 크기 (최소 2 필요).
   * @returns 인코딩된 총 바이트 수, 오류 시 에러 코드.
   */
  Result<size_t> encode(const std::string &line, iovec *vecs,
                         size_t max_vecs) override {
    if (max_vecs < 2)
      return unexpected(std::make_error_code(std::errc::no_buffer_space));

    vecs[0].iov_base = const_cast<char *>(line.data());
    vecs[0].iov_len  = line.size();

    // 구분자 — 정적 버퍼 사용
    static const char kLF[]   = "\n";
    static const char kCRLF[] = "\r\n";
    if (crlf_) {
      vecs[1].iov_base = const_cast<char *>(kCRLF);
      vecs[1].iov_len  = 2;
    } else {
      vecs[1].iov_base = const_cast<char *>(kLF);
      vecs[1].iov_len  = 1;
    }

    return line.size() + (crlf_ ? 2u : 1u);
  }

private:
  /** @brief CRLF 모드 플래그 (`\r\n` 구분자 사용 여부). */
  bool        crlf_;
  /** @brief 불완전한 줄 누적 버퍼. */
  std::string partial_;
};

// ─── Http1Codec ──────────────────────────────────────────────────────────────

/**
 * @brief HTTP/1.1 요청 파서 / 응답 인코더 코덱.
 *
 * `HttpParser`를 래핑하여 `IFrameCodec<http::Request>` 인터페이스를 제공합니다.
 * 요청을 디코딩하고 응답(`http::Response`)을 인코딩합니다.
 *
 * ### 주의
 * - `encode()`는 `Request`가 아닌 `Response`를 인코딩합니다.
 *   `IFrameCodec<Request>::encode`의 frame 인자는 사용되지 않습니다.
 * - 응답 인코딩은 `encode_response()`를 사용하세요.
 *
 * @code
 * Http1Codec codec;
 * http::Request req;
 * int n = codec.decode(buf, req);
 * if (n > 0) {
 *   // 완성된 HTTP 요청 처리
 *   http::Response resp;
 *   resp.status(200).body("OK");
 *   iovec vecs[4];
 *   auto sz = codec.encode_response(resp, vecs, 4);
 * }
 * @endcode
 */
class Http1Codec : public IFrameCodec<Request> {
public:
  Http1Codec() = default;

  /**
   * @brief 버퍼에서 HTTP/1.1 요청을 디코딩합니다.
   *
   * 내부 `HttpParser`를 재사용합니다.
   * 완성된 요청 이후 `reset()`을 호출하면 다음 요청을 파싱할 수 있습니다.
   *
   * @param buf 원시 바이트 버퍼.
   * @param out 디코딩된 HTTP 요청.
   * @returns 소비한 바이트 수 (>0 = 완성), 0 = 불완전, -1 = 파싱 오류.
   */
  int decode(std::span<const std::byte> buf, Request &out) override {
    std::string_view sv(reinterpret_cast<const char *>(buf.data()), buf.size());
    auto consumed = parser_.parse(sv, out);
    if (!consumed.has_value()) return -1; // 파싱 오류
    if (!parser_.is_complete()) return 0; // 불완전
    return static_cast<int>(*consumed);
  }

  /**
   * @brief `IFrameCodec<Request>::encode` — 사용하지 않습니다.
   *
   * HTTP 서버는 요청을 인코딩하지 않습니다.
   * 응답 인코딩은 `encode_response()`를 사용하세요.
   *
   * @returns 항상 errc::operation_not_supported 오류.
   */
  Result<size_t> encode(const Request & /*frame*/, iovec * /*vecs*/,
                         size_t /*max_vecs*/) override {
    return unexpected(
        std::make_error_code(std::errc::operation_not_supported));
  }

  /**
   * @brief HTTP 응답을 iovec 배열로 인코딩합니다.
   *
   * iovec[0] = 직렬화된 헤더 (상태 줄 + 헤더 섹션),
   * iovec[1] = 바디 (비어 있으면 iov_len = 0).
   *
   * @param resp     인코딩할 HTTP 응답.
   * @param vecs     출력 iovec 배열.
   * @param max_vecs iovec 배열의 최대 크기 (최소 2 필요).
   * @returns 인코딩된 총 바이트 수, 오류 시 에러 코드.
   */
  Result<size_t> encode_response(const Response &resp, iovec *vecs,
                                  size_t max_vecs) {
    if (max_vecs < 2)
      return unexpected(std::make_error_code(std::errc::no_buffer_space));

    // 헤더를 내부 버퍼에 직렬화
    header_buf_ = resp.serialize_header();
    body_buf_   = std::string(resp.get_body());

    vecs[0].iov_base = header_buf_.data();
    vecs[0].iov_len  = header_buf_.size();
    vecs[1].iov_base = body_buf_.data();
    vecs[1].iov_len  = body_buf_.size();

    return header_buf_.size() + body_buf_.size();
  }

  /**
   * @brief 파서를 리셋합니다 (다음 요청 파싱 준비).
   */
  void reset() {
    parser_ = HttpParser{};
  }

  /**
   * @brief 헤더 파싱 완료 여부를 반환합니다.
   *
   * 100-Continue 응답 전송 시점 판단에 유용합니다.
   */
  [[nodiscard]] bool headers_complete() const noexcept {
    return parser_.headers_complete();
  }

  /**
   * @brief 파싱 오류 시 반환할 HTTP 상태 코드를 반환합니다.
   *
   * - 400: 잘못된 요청 구문
   * - 413: 페이로드 너무 큼
   */
  [[nodiscard]] int error_status() const noexcept {
    return parser_.error_status();
  }

private:
  /** @brief HTTP/1.1 요청 파서 (상태 머신). */
  HttpParser   parser_;
  /** @brief encode_response()가 채우는 직렬화된 헤더 버퍼. */
  std::string  header_buf_;
  /** @brief encode_response()가 채우는 바디 버퍼. */
  std::string  body_buf_;
};

} // namespace qbuem

/** @} */ // end of qbuem_codec
