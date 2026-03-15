#pragma once

/**
 * @file qbuem/http/inline_request.hpp
 * @brief 소형 요청 헤더용 스택 기반 인라인 버퍼 최적화
 * @defgroup qbuem_inline_request Inline Request Buffer
 * @ingroup qbuem_http
 *
 * 대부분의 HTTP 요청 헤더는 2 KiB 이하입니다 (일반 GET, POST).
 * `InlineRequestBuffer<N>` 은 N 바이트 이하의 헤더를 힙 할당 없이
 * 스택(또는 인라인 배열)에 저장합니다.
 *
 * ## 적용 포인트
 * HTTP 파서가 원시 바이트를 수신하면:
 * 1. 헤더가 `N` 바이트 이하이면 → `InlineRequestBuffer<N>`에 직접 저장
 * 2. 헤더가 `N` 초과이면 → 기존 `std::string` 힙 할당으로 폴백
 *
 * ## 사용 예시
 * ```cpp
 * // 수신된 원시 HTTP 데이터
 * std::string_view raw = "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";
 *
 * qbuem::http::InlineRequestBuffer<2048> buf;
 * bool fits = buf.try_push(raw);
 * if (fits) {
 *     // 힙 할당 없이 파싱
 *     HttpParser parser;
 *     Request req;
 *     parser.parse(buf.view(), req);
 * }
 * ```
 *
 * @{
 */

#include <array>
#include <cstddef>
#include <cstring>
#include <string_view>

namespace qbuem::http {

/**
 * @brief 고정 크기 인라인 버퍼 — 소형 HTTP 헤더 힙 회피.
 *
 * @tparam N  인라인 버퍼 크기 (바이트). 기본 2 KiB.
 *            헤더가 N 초과이면 `try_push()`가 false를 반환합니다.
 */
template <size_t N = 2048>
class InlineRequestBuffer {
public:
  /**
   * @brief 데이터를 인라인 버퍼에 추가합니다.
   *
   * @param data 추가할 바이트 슬라이스.
   * @returns true — 버퍼에 수용됨. false — N 초과로 힙 할당 필요.
   */
  bool try_push(std::string_view data) noexcept {
    if (used_ + data.size() > N) return false;
    std::memcpy(buf_.data() + used_, data.data(), data.size());
    used_ += data.size();
    return true;
  }

  /**
   * @brief 버퍼에 단일 바이트를 추가합니다.
   */
  bool try_push(char c) noexcept {
    if (used_ >= N) return false;
    buf_[used_++] = static_cast<std::byte>(c);
    return true;
  }

  /**
   * @brief 현재까지 저장된 내용의 뷰를 반환합니다.
   */
  [[nodiscard]] std::string_view view() const noexcept {
    return std::string_view{
        reinterpret_cast<const char*>(buf_.data()), used_};
  }

  /// @brief 저장된 바이트 수.
  [[nodiscard]] size_t size() const noexcept { return used_; }

  /// @brief 남은 가용 공간 (바이트).
  [[nodiscard]] size_t remaining() const noexcept { return N - used_; }

  /// @brief 버퍼가 비어 있는지 확인합니다.
  [[nodiscard]] bool empty() const noexcept { return used_ == 0; }

  /// @brief 버퍼를 초기화합니다 (재사용).
  void reset() noexcept { used_ = 0; }

  /// @brief 인라인 버퍼의 최대 크기.
  static constexpr size_t capacity() noexcept { return N; }

private:
  std::array<std::byte, N> buf_{};
  size_t                   used_{0};
};

/**
 * @brief 소형/대형 요청을 자동 선택하는 적응형 버퍼.
 *
 * `N` 바이트 이하이면 인라인 스택 버퍼를 사용하고,
 * 초과 시 `std::string`으로 폴백합니다.
 *
 * @tparam N  인라인 임계값 (기본 2 KiB).
 */
template <size_t N = 2048>
class AdaptiveRequestBuffer {
public:
  /**
   * @brief 데이터를 버퍼에 추가합니다.
   * @param data 추가할 바이트.
   */
  void push(std::string_view data) {
    if (using_inline_) {
      if (inline_buf_.try_push(data)) return;
      // 오버플로 → 힙 폴백
      heap_buf_.reserve(inline_buf_.size() + data.size());
      heap_buf_.append(
          reinterpret_cast<const char*>(inline_buf_.view().data()),
          inline_buf_.size());
      heap_buf_.append(data.data(), data.size());
      using_inline_ = false;
    } else {
      heap_buf_.append(data.data(), data.size());
    }
  }

  /**
   * @brief 버퍼 내용의 뷰를 반환합니다.
   */
  [[nodiscard]] std::string_view view() const noexcept {
    if (using_inline_) return inline_buf_.view();
    return heap_buf_;
  }

  /// @brief 힙 할당을 사용 중인지 반환합니다.
  [[nodiscard]] bool is_heap() const noexcept { return !using_inline_; }

  /// @brief 저장된 총 바이트 수.
  [[nodiscard]] size_t size() const noexcept {
    return using_inline_ ? inline_buf_.size() : heap_buf_.size();
  }

  /// @brief 버퍼를 초기화합니다.
  void reset() noexcept {
    inline_buf_.reset();
    heap_buf_.clear();
    using_inline_ = true;
  }

private:
  InlineRequestBuffer<N> inline_buf_;
  std::string            heap_buf_;
  bool                   using_inline_{true};
};

} // namespace qbuem::http

/** @} */
