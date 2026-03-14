#pragma once

/**
 * @file qbuem/buf/io_slice.hpp
 * @brief 힙 할당 없는 I/O 버퍼 기본 타입 집합.
 * @defgroup qbuem_buf Buffer Primitives
 * @ingroup qbuem_buf
 *
 * 이 헤더는 zero-alloc I/O를 위한 핵심 버퍼 추상화를 제공합니다:
 *
 * - `IOSlice`     : 읽기 전용 fat pointer (const std::byte* + size)
 * - `IOVec<N>`    : 스택 기반 scatter-gather iovec 배열
 * - `ReadBuf<N>`  : 고정 크기 링 버퍼 (컴파일 타임 크기)
 * - `WriteBuf`    : arena 기반 cork 버퍼 (청크 단위 append)
 *
 * ### 설계 원칙
 * - `IOSlice`, `IOVec<N>`, `ReadBuf<N>`: 스택 전용, 힙 할당 없음
 * - `WriteBuf`: 내부적으로 청크 벡터를 사용하지만 외부 API는 iovec 배열로 제공
 * @{
 */

#include <qbuem/common.hpp>

#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>

namespace qbuem {

// ─── IOSlice ─────────────────────────────────────────────────────────────────

/**
 * @brief 읽기 전용 바이트 fat pointer.
 *
 * `const std::byte*`와 `size_t`를 하나의 값 타입으로 묶습니다.
 * 데이터 복사 없이 기존 버퍼를 참조합니다.
 *
 * @warning 참조하는 버퍼의 수명이 IOSlice보다 길어야 합니다.
 */
struct IOSlice {
  /** @brief 데이터 포인터. */
  const std::byte *data = nullptr;

  /** @brief 데이터 크기 (바이트). */
  size_t size = 0;

  /** @brief 기본 생성자. */
  IOSlice() = default;

  /**
   * @brief 포인터와 크기로 IOSlice를 구성합니다.
   * @param d 데이터 포인터.
   * @param s 데이터 크기 (바이트).
   */
  IOSlice(const std::byte *d, size_t s) noexcept : data(d), size(s) {}

  /**
   * @brief std::span<const std::byte>으로부터 IOSlice를 구성합니다.
   * @param s 원본 span.
   */
  IOSlice(std::span<const std::byte> s) noexcept  // NOLINT
      : data(s.data()), size(s.size()) {}

  /**
   * @brief std::span<const std::byte>으로 암묵적 변환합니다.
   * @returns 동일 범위를 가리키는 span.
   */
  operator std::span<const std::byte>() const noexcept {  // NOLINT
    return {data, size};
  }
};

// ─── IOVec<N> ────────────────────────────────────────────────────────────────

/**
 * @brief 스택 기반 scatter-gather iovec 배열.
 *
 * `writev(2)` / `readv(2)` 에 직접 전달할 수 있는 iovec 배열을
 * 스택에 고정 크기로 관리합니다. 힙 할당이 전혀 발생하지 않습니다.
 *
 * @tparam N 최대 iovec 원소 수 (컴파일 타임 상수).
 *
 * ### 사용 예시
 * @code
 * IOVec<4> iov;
 * iov.push(header.data(), header.size());
 * iov.push(body.data(), body.size());
 * iov.writev_to(fd);
 * @endcode
 */
template <size_t N>
struct IOVec {
  /** @brief iovec 원소 배열. */
  iovec vecs[N];

  /** @brief 현재 저장된 원소 수. */
  size_t count = 0;

  /**
   * @brief 포인터와 크기로 iovec 원소를 추가합니다.
   *
   * @param base 데이터 시작 포인터.
   * @param len  데이터 크기 (바이트).
   * @note `count >= N` 이면 무시됩니다 (오버플로 방지).
   */
  void push(const void *base, size_t len) noexcept {
    if (count < N) {
      vecs[count].iov_base = const_cast<void *>(base);
      vecs[count].iov_len  = len;
      ++count;
    }
  }

  /**
   * @brief IOSlice로부터 iovec 원소를 추가합니다.
   * @param s 추가할 IOSlice.
   */
  void push(IOSlice s) noexcept { push(s.data, s.size); }

  /**
   * @brief iovec 배열의 포인터를 반환합니다.
   * @returns `vecs` 배열의 첫 번째 원소 포인터.
   */
  iovec *data() noexcept { return vecs; }

  /**
   * @brief iovec 배열의 const 포인터를 반환합니다.
   * @returns `vecs` 배열의 첫 번째 원소 const 포인터.
   */
  const iovec *data() const noexcept { return vecs; }

  /**
   * @brief 현재 저장된 iovec 원소 수를 반환합니다.
   * @returns 0 ~ N 범위의 원소 수.
   */
  size_t size() const noexcept { return count; }

  /**
   * @brief 저장된 모든 iovec 버퍼를 fd에 writev(2)로 씁니다.
   *
   * @param fd 대상 파일 디스크립터.
   * @returns 전송한 바이트 수. 에러 시 -1.
   */
  ssize_t writev_to(int fd) const noexcept {
    return ::writev(fd, vecs, static_cast<int>(count));
  }
};

// ─── ReadBuf<N> ──────────────────────────────────────────────────────────────

/**
 * @brief 고정 크기 링 버퍼 (컴파일 타임 크기).
 *
 * 스택 또는 구조체 멤버로 사용할 수 있는 링 버퍼입니다.
 * 힙 할당이 전혀 발생하지 않으며, 읽기/쓰기 위치를 선형으로 증가시켜
 * 간단하고 효율적으로 관리합니다.
 *
 * @tparam N 버퍼 크기 (바이트, 컴파일 타임 상수).
 *
 * ### 사용 예시
 * @code
 * ReadBuf<4096> rbuf;
 * ssize_t n = ::read(fd, rbuf.write_head(), rbuf.writable());
 * rbuf.commit(n);
 * auto data = rbuf.readable(); // 파싱에 사용
 * rbuf.consume(parsed_bytes);
 * @endcode
 */
template <size_t N>
class ReadBuf {
public:
  /**
   * @brief 쓰기 헤드 포인터를 반환합니다.
   *
   * `read(2)` 등의 syscall에 직접 전달하여 이 위치에 데이터를 씁니다.
   *
   * @returns 현재 쓰기 위치 포인터.
   */
  std::byte *write_head() noexcept { return buf_ + write_pos_; }

  /**
   * @brief 쓸 수 있는 여유 공간 크기를 반환합니다.
   *
   * @returns `N - write_pos_` (바이트).
   * @note 이 구현은 선형 버퍼 방식을 사용합니다.
   *       `consume()` 호출 후 `compact()`로 공간을 회수할 수 있습니다.
   */
  size_t writable() const noexcept {
    return N > write_pos_ ? N - write_pos_ : 0;
  }

  /**
   * @brief 쓰기 위치를 n바이트 전진합니다.
   *
   * `read(2)` 등이 실제로 쓴 바이트 수를 인자로 전달합니다.
   *
   * @param n 전진할 바이트 수.
   * @note `n > writable()` 이면 `write_pos_`를 N으로 클램프합니다.
   */
  void commit(size_t n) noexcept {
    write_pos_ += n;
    if (write_pos_ > N) write_pos_ = N;
  }

  /**
   * @brief 읽을 수 있는 데이터의 span을 반환합니다.
   *
   * @returns `[read_pos_, write_pos_)` 범위의 데이터 뷰.
   */
  std::span<const std::byte> readable() const noexcept {
    return {buf_ + read_pos_, write_pos_ - read_pos_};
  }

  /**
   * @brief 읽기 위치를 n바이트 전진합니다 (소비).
   *
   * @param n 소비할 바이트 수.
   * @note `n > size()` 이면 `read_pos_`를 `write_pos_`로 클램프합니다.
   */
  void consume(size_t n) noexcept {
    read_pos_ += n;
    if (read_pos_ > write_pos_) read_pos_ = write_pos_;
    // 버퍼가 비어있으면 위치를 리셋하여 가용 공간을 최대화
    if (read_pos_ == write_pos_) {
      read_pos_ = 0;
      write_pos_ = 0;
    }
  }

  /**
   * @brief 읽을 데이터가 없는지 확인합니다.
   * @returns `size() == 0`이면 true.
   */
  bool empty() const noexcept { return read_pos_ == write_pos_; }

  /**
   * @brief 읽을 수 있는 데이터의 바이트 수를 반환합니다.
   * @returns `write_pos_ - read_pos_`.
   */
  size_t size() const noexcept { return write_pos_ - read_pos_; }

  /**
   * @brief 버퍼 전체 용량을 반환합니다.
   * @returns 템플릿 파라미터 N.
   */
  static constexpr size_t capacity() noexcept { return N; }

private:
  /** @brief 내부 바이트 버퍼. */
  std::byte buf_[N]{};

  /** @brief 읽기 위치 (소비된 바이트 오프셋). */
  size_t read_pos_ = 0;

  /** @brief 쓰기 위치 (커밋된 바이트 오프셋). */
  size_t write_pos_ = 0;
};

// ─── WriteBuf ────────────────────────────────────────────────────────────────

/**
 * @brief arena 기반 cork 버퍼.
 *
 * 여러 데이터 청크를 추가(append)한 뒤 `as_iovec()`으로 iovec 배열을 구성하여
 * `writev(2)` 한 번에 모두 전송하는 "cork" 패턴을 지원합니다.
 *
 * ### 사용 예시
 * @code
 * WriteBuf wbuf;
 * wbuf.append(header_bytes);
 * wbuf.append("HTTP/1.1 200 OK\r\n");
 * wbuf.append(body);
 *
 * iovec iov[64];
 * size_t n = wbuf.as_iovec(iov, 64);
 * ::writev(fd, iov, static_cast<int>(n));
 * wbuf.clear();
 * @endcode
 */
class WriteBuf {
public:
  /**
   * @brief 읽기 전용 바이트 span을 버퍼에 추가합니다.
   *
   * 데이터는 내부 청크로 복사됩니다.
   *
   * @param data 추가할 데이터.
   */
  void append(std::span<const std::byte> data) {
    if (data.empty()) return;
    chunks_.push_back(Chunk{std::vector<std::byte>(data.begin(), data.end())});
    total_ += data.size();
  }

  /**
   * @brief 문자열 뷰를 버퍼에 추가합니다.
   *
   * @param sv 추가할 문자열 데이터.
   */
  void append(std::string_view sv) {
    if (sv.empty()) return;
    const auto *ptr = reinterpret_cast<const std::byte *>(sv.data());
    append(std::span<const std::byte>{ptr, sv.size()});
  }

  /**
   * @brief 버퍼에 저장된 총 바이트 수를 반환합니다.
   * @returns 모든 청크의 크기 합계.
   */
  size_t size() const noexcept { return total_; }

  /**
   * @brief 저장된 청크가 없는지 확인합니다.
   * @returns `total_ == 0`이면 true.
   */
  bool empty() const noexcept { return total_ == 0; }

  /**
   * @brief iovec 배열을 채웁니다.
   *
   * 각 청크가 하나의 iovec 원소로 매핑됩니다.
   * `writev(2)` 호출 전에 사용합니다.
   *
   * @param out   iovec 배열 포인터.
   * @param max_n 배열의 최대 원소 수.
   * @returns 실제 채워진 iovec 원소 수 (min(청크 수, max_n)).
   */
  size_t as_iovec(iovec *out, size_t max_n) const noexcept {
    size_t n = 0;
    for (const auto &c : chunks_) {
      if (n >= max_n) break;
      out[n].iov_base = const_cast<std::byte *>(c.data.data());
      out[n].iov_len  = c.data.size();
      ++n;
    }
    return n;
  }

  /**
   * @brief 모든 청크를 삭제하고 버퍼를 초기화합니다.
   */
  void clear() noexcept {
    chunks_.clear();
    total_ = 0;
  }

private:
  /**
   * @brief 단일 데이터 청크.
   *
   * `append()` 호출마다 하나의 Chunk가 생성됩니다.
   */
  struct Chunk {
    std::vector<std::byte> data; ///< 청크 데이터
  };

  /** @brief 청크 목록. */
  std::vector<Chunk> chunks_;

  /** @brief 전체 바이트 수 캐시. */
  size_t total_ = 0;
};

} // namespace qbuem

/** @} */ // end of qbuem_buf
