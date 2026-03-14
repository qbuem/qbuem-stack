#pragma once

/**
 * @file qbuem/codec/length_prefix_codec.hpp
 * @brief 4바이트 빅엔디안 길이 접두사 프레임 코덱
 * @ingroup qbuem_codec
 *
 * 이 헤더는 `IFrameCodec<LengthPrefixedFrame>` 구체 구현을 제공합니다.
 *
 * ## 프레임 형식
 * ```
 * ┌─────────────────────────────┬───────────────────────────────┐
 * │  length (4 bytes, big-endian) │   payload (length bytes)    │
 * └─────────────────────────────┴───────────────────────────────┘
 * ```
 *
 * ## 사용 예시
 * @code
 * LengthPrefixedCodec codec;
 * LengthPrefixedFrame frame;
 * auto status = codec.decode(recv_buf, frame);
 * if (status == DecodeStatus::Complete) {
 *   // frame.payload에 페이로드 데이터 존재
 *   process(frame.payload);
 *   codec.reset();
 * }
 * @endcode
 *
 * @{
 */

#include <qbuem/codec/frame_codec.hpp>

#include <arpa/inet.h> // ntohl, htonl
#include <cstring>
#include <memory_resource>
#include <vector>

namespace qbuem::codec {

// ─── LengthPrefixedFrame ──────────────────────────────────────────────────────

/**
 * @brief 길이 접두사 프로토콜의 단일 프레임.
 *
 * `length`는 호스트 바이트 순서(host byte order)로 저장됩니다.
 * 네트워크로 전송 시 `LengthPrefixedCodec::encode()`가 자동으로 빅엔디안 변환합니다.
 */
struct LengthPrefixedFrame {
  /** @brief 페이로드 길이 (호스트 바이트 순서). encode() 시 빅엔디안으로 직렬화됨. */
  uint32_t length = 0;

  /** @brief 페이로드 데이터. */
  std::vector<std::byte> payload;
};

// ─── LengthPrefixedCodec ──────────────────────────────────────────────────────

/**
 * @brief 4바이트 빅엔디안 길이 접두사 기반 프레임 코덱.
 *
 * `IFrameCodec<LengthPrefixedFrame>`을 구현합니다.
 * 내부적으로 헤더/바디 파싱 상태 머신을 유지합니다.
 *
 * ### 상태 머신
 * ```
 * Header (4바이트 수신 대기)
 *   → Body (length 바이트 수신 대기)
 *     → Complete (프레임 완성, reset() 호출로 Header로 복귀)
 * ```
 *
 * ### 인코딩
 * `encode()`는 iovec[0]에 4바이트 빅엔디안 헤더를, iovec[1]에 페이로드를 설정합니다.
 * `arena`를 통해 헤더 버퍼를 할당하거나 nullptr 시 내부 버퍼를 사용합니다.
 */
class LengthPrefixedCodec : public IFrameCodec<LengthPrefixedFrame> {
public:
  /** @brief 기본 생성자. 상태를 Header로 초기화합니다. */
  LengthPrefixedCodec() = default;

  /**
   * @brief 버퍼에서 길이 접두사 프레임을 디코딩합니다.
   *
   * 두 단계로 처리됩니다:
   * 1. Header 단계: 4바이트 빅엔디안 길이 값을 읽습니다.
   * 2. Body 단계: `pending_length_` 바이트를 읽어 페이로드를 완성합니다.
   *
   * 성공 시 `buf`는 소비된 바이트만큼 앞으로 이동합니다.
   *
   * @param[in,out] buf 원시 바이트 뷰. `Complete` 시 소비 바이트만큼 이동.
   * @param[out]    out 디코딩된 프레임. `Complete` 시에만 유효.
   * @returns `Complete` | `Incomplete` | `Error`.
   */
  DecodeStatus decode(BufferView &buf, LengthPrefixedFrame &out) override {
    size_t consumed = 0;

    if (state_ == ParseState::Header) {
      // 헤더 버퍼에 데이터 누적
      while (header_received_ < kHeaderSize && consumed < buf.size()) {
        header_buf_[header_received_++] = buf[consumed++];
      }

      if (header_received_ < kHeaderSize) {
        buf = buf.subspan(consumed);
        return DecodeStatus::Incomplete;
      }

      // 4바이트 빅엔디안 → 호스트 바이트 순서 변환
      uint32_t net_len = 0;
      std::memcpy(&net_len, header_buf_, kHeaderSize);
      pending_length_ = ntohl(net_len);

      state_ = ParseState::Body;
      body_buf_.clear();
      body_buf_.reserve(pending_length_);
    }

    // Body 단계: pending_length_ 바이트 수신
    size_t remaining = pending_length_ - body_buf_.size();
    size_t available = buf.size() - consumed;
    size_t to_copy   = std::min(remaining, available);

    const auto *src = reinterpret_cast<const std::byte *>(buf.data() + consumed);
    body_buf_.insert(body_buf_.end(), src, src + to_copy);
    consumed += to_copy;

    buf = buf.subspan(consumed);

    if (body_buf_.size() < pending_length_) {
      return DecodeStatus::Incomplete;
    }

    // 프레임 완성
    out.length  = pending_length_;
    out.payload = std::move(body_buf_);
    return DecodeStatus::Complete;
  }

  /**
   * @brief 프레임을 iovec 배열로 인코딩합니다.
   *
   * iovec[0] = 4바이트 빅엔디안 길이 헤더 (arena 또는 내부 버퍼 사용)
   * iovec[1] = 페이로드 데이터 (zero-copy)
   *
   * `max_vecs`는 최소 2 이상이어야 합니다.
   *
   * @param frame    인코딩할 프레임.
   * @param vecs     출력 iovec 배열.
   * @param max_vecs iovec 배열 크기 (최소 2 필요).
   * @param arena    헤더 버퍼 할당에 사용할 메모리 리소스. nullptr 시 내부 버퍼 사용.
   * @returns 사용된 iovec 항목 수 (2). `max_vecs < 2`이면 0.
   */
  size_t encode(const LengthPrefixedFrame &frame, iovec *vecs, size_t max_vecs,
                std::pmr::memory_resource *arena) override {
    if (max_vecs < 2) return 0;

    // 헤더 버퍼: arena가 있으면 arena에서, 없으면 내부 버퍼 사용
    uint8_t *hdr_buf;
    if (arena) {
      hdr_buf = static_cast<uint8_t *>(arena->allocate(kHeaderSize, alignof(uint32_t)));
    } else {
      hdr_buf = encode_hdr_buf_;
    }

    uint32_t net_len = htonl(frame.length);
    std::memcpy(hdr_buf, &net_len, kHeaderSize);

    vecs[0].iov_base = hdr_buf;
    vecs[0].iov_len  = kHeaderSize;
    vecs[1].iov_base = const_cast<std::byte *>(frame.payload.data());
    vecs[1].iov_len  = frame.payload.size();

    return 2;
  }

  /**
   * @brief 파서 상태를 초기화합니다.
   *
   * 다음 프레임 파싱을 위해 Header 단계로 돌아갑니다.
   */
  void reset() override {
    state_            = ParseState::Header;
    pending_length_   = 0;
    header_received_  = 0;
    body_buf_.clear();
  }

private:
  /** @brief 길이 헤더 크기 (바이트). */
  static constexpr size_t kHeaderSize = 4;

  /** @brief 파서 단계를 나타내는 상태 열거형. */
  enum class ParseState { Header, Body };

  /** @brief 현재 파서 상태. */
  ParseState state_ = ParseState::Header;

  /** @brief 헤더 수신 버퍼 (4바이트). */
  uint8_t  header_buf_[kHeaderSize] = {};

  /** @brief 현재까지 수신된 헤더 바이트 수. */
  size_t   header_received_ = 0;

  /** @brief 수신 예정인 페이로드 길이 (호스트 바이트 순서). */
  uint32_t pending_length_ = 0;

  /** @brief 페이로드 수신 버퍼. */
  std::vector<std::byte> body_buf_;

  /** @brief arena가 nullptr일 때 encode()에서 사용하는 내부 헤더 버퍼. */
  uint8_t encode_hdr_buf_[kHeaderSize] = {};
};

} // namespace qbuem::codec

/** @} */ // end of qbuem_codec (length_prefix)
