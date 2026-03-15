#pragma once

/**
 * @file qbuem/server/websocket_handler.hpp
 * @brief RFC 6455 WebSocket 핸들러 — HTTP/1.1 업그레이드 핸드셰이크 및 프레임 처리
 * @defgroup qbuem_websocket_handler WebSocket Handler
 * @ingroup qbuem_server
 *
 * 이 헤더는 RFC 6455 WebSocket 프로토콜의 핵심 구현을 제공합니다.
 *
 * ### 주요 기능
 * - **HTTP/1.1 업그레이드 핸드셰이크**: `101 Switching Protocols` 응답 생성.
 * - **`Sec-WebSocket-Accept` 계산**: RFC 6455 §4.2.2의 SHA-1 + Base64 키 유도.
 * - **프레임 인코딩/디코딩**: 가변 길이 페이로드, 마스킹(RFC 6455 §5.3) 지원.
 * - **제어 프레임**: Ping/Pong/Close 자동 처리.
 * - **메시지 핸들러 주입**: 생성자에서 콜백으로 수신 프레임을 애플리케이션으로 전달.
 *
 * ### SHA-1 주의사항
 * WebSocket 핸드셰이크의 `Sec-WebSocket-Accept` 계산에는 SHA-1이 필요합니다.
 * 이 구현은 RFC 3174 기반의 인라인 SHA-1을 포함합니다. 외부 의존성이 없으며
 * TLS 보안 목적이 아닌 핸드셰이크 전용으로만 사용됩니다.
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/http/request.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace qbuem {

// ─── WsFrame ─────────────────────────────────────────────────────────────────

/**
 * @brief RFC 6455 WebSocket 프레임.
 *
 * WebSocket 프로토콜의 기본 데이터 단위입니다.
 * `WebSocketHandler`가 디코딩한 프레임을 애플리케이션 콜백으로 전달합니다.
 *
 * ### 멀티프레임 메시지 (프래그먼테이션)
 * - 첫 번째 프레임: `fin == false`, opcode == Text 또는 Binary.
 * - 중간 프레임: `fin == false`, opcode == Continuation.
 * - 마지막 프레임: `fin == true`, opcode == Continuation.
 */
struct WsFrame {
  /**
   * @brief WebSocket 프레임 opcode (RFC 6455 §5.2).
   */
  enum class Opcode : uint8_t {
    Continuation = 0x0, ///< 이전 프레임의 연속 (프래그먼테이션)
    Text         = 0x1, ///< UTF-8 텍스트 데이터
    Binary       = 0x2, ///< 임의 바이너리 데이터
    Close        = 0x8, ///< 연결 종료 요청
    Ping         = 0x9, ///< Ping (연결 유지 확인)
    Pong         = 0xA, ///< Ping에 대한 응답
  };

  /** @brief 프레임 타입을 나타내는 opcode. */
  Opcode opcode{Opcode::Text};

  /** @brief 최종 프레임 여부. false이면 멀티프레임 메시지의 일부. */
  bool fin{true};

  /** @brief 페이로드 마스킹 여부. 클라이언트 → 서버 방향은 반드시 true여야 합니다. */
  bool masked{false};

  /** @brief 프레임 페이로드 데이터. 마스킹이 적용된 경우 이미 언마스킹된 상태입니다. */
  std::vector<uint8_t> payload;
};

// ─── WebSocketHandler ─────────────────────────────────────────────────────────

/**
 * @brief RFC 6455 WebSocket 서버 핸들러.
 *
 * HTTP/1.1 Upgrade 핸드셰이크부터 이진/텍스트 프레임 송수신까지
 * WebSocket 프로토콜의 서버 측 처리를 담당합니다.
 *
 * ### 사용 예시
 * @code
 * auto ws_handler = std::make_shared<WebSocketHandler>(
 *     [](WsFrame frame) -> Task<void> {
 *         if (frame.opcode == WsFrame::Opcode::Text) {
 *             std::string text(frame.payload.begin(), frame.payload.end());
 *             // echo back
 *         }
 *         co_return;
 *     }
 * );
 *
 * // Http1Handler의 업그레이드 콜백으로 연결
 * auto http_handler = std::make_unique<Http1Handler>(router,
 *     [ws_handler](UpgradeRequest req) -> Task<void> {
 *         auto result = co_await ws_handler->upgrade(req.fd, req.original_request);
 *         if (result) co_await ws_handler->run(req.fd);
 *     }
 * );
 * @endcode
 */
class WebSocketHandler {
public:
  /**
   * @brief 수신 WebSocket 프레임을 처리하는 콜백 타입.
   *
   * `run()` 루프가 완전한 프레임을 수신할 때마다 호출됩니다.
   * 제어 프레임(Ping, Close)은 자동으로 처리되며 콜백으로 전달되지 않습니다.
   */
  using MessageHandler = std::function<Task<void>(WsFrame)>;

  /**
   * @brief WebSocketHandler를 구성합니다.
   *
   * @param on_message 수신 프레임마다 호출될 콜백.
   */
  explicit WebSocketHandler(MessageHandler on_message)
      : on_message_(std::move(on_message)) {}

  // ── 업그레이드 핸드셰이크 ───────────────────────────────────────────────

  /**
   * @brief HTTP/1.1 WebSocket 업그레이드 핸드셰이크를 수행합니다.
   *
   * RFC 6455 §4.2.2 절차:
   * 1. `Sec-WebSocket-Key` 헤더 검증.
   * 2. `Sec-WebSocket-Accept` 키 계산 (SHA-1 + Base64).
   * 3. `101 Switching Protocols` 응답 전송.
   *
   * @param fd  연결된 소켓 파일 디스크립터.
   * @param req 업그레이드를 요청한 원본 HTTP 요청.
   * @returns 성공 시 `Result<void>::ok()`, 유효하지 않은 요청이면 에러 코드.
   */
  Task<Result<void>> upgrade(int fd, const http::Request& req) {
    std::string_view ws_key = req.header("Sec-WebSocket-Key");
    if (ws_key.empty()) {
      // 400 Bad Request: Sec-WebSocket-Key 누락
      static constexpr std::string_view k400 =
          "HTTP/1.1 400 Bad Request\r\n"
          "Connection: close\r\n"
          "Content-Length: 0\r\n"
          "\r\n";
      write_all(fd, k400);
      co_return unexpected(
          std::make_error_code(std::errc::invalid_argument));
    }

    std::string accept_key = compute_accept_key(ws_key);

    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept_key + "\r\n"
        "\r\n";

    if (!write_all(fd, response)) {
      co_return unexpected(
          std::error_code(errno, std::system_category()));
    }

    co_return Result<void>::ok();
  }

  // ── 프레임 송수신 루프 ──────────────────────────────────────────────────

  /**
   * @brief WebSocket 프레임 수신/디스패치 루프를 실행합니다.
   *
   * `upgrade()` 성공 후 호출합니다. 연결이 종료될 때까지 다음을 반복합니다:
   * - 소켓에서 프레임 읽기.
   * - Ping → Pong 자동 응답.
   * - Close → Close 에코 후 루프 종료.
   * - Text/Binary/Continuation → `on_message_` 콜백 호출.
   *
   * @param fd 연결된 소켓 파일 디스크립터.
   */
  Task<void> run(int fd) {
    std::vector<uint8_t> buf;
    buf.reserve(4096);

    for (;;) {
      // 소켓에서 데이터 수신 (단순화: 최대 64KB 청크)
      uint8_t tmp[65536];
      ssize_t n = ::read(fd, tmp, sizeof(tmp));
      if (n <= 0) break; // EOF 또는 에러

      buf.insert(buf.end(), tmp, tmp + n);

      // 버퍼에서 완전한 프레임 추출
      size_t consumed = 0;
      for (;;) {
        auto span = std::span<const uint8_t>(buf.data() + consumed,
                                             buf.size() - consumed);
        size_t frame_consumed = 0;
        auto result = decode_frame(span, frame_consumed);
        if (!result.has_value()) break; // 프레임 미완성

        WsFrame frame = std::move(result.value());
        consumed += frame_consumed;

        // 제어 프레임 자동 처리
        if (frame.opcode == WsFrame::Opcode::Ping) {
          // Pong 응답 전송
          auto pong_bytes = encode_frame(
              WsFrame{WsFrame::Opcode::Pong, true, false, {}}, false);
          write_all(fd, std::string_view(
              reinterpret_cast<const char*>(pong_bytes.data()),
              pong_bytes.size()));
          continue;
        }

        if (frame.opcode == WsFrame::Opcode::Close) {
          // Close 에코 후 루프 종료
          auto close_bytes = encode_frame(
              WsFrame{WsFrame::Opcode::Close, true, false,
                      frame.payload}, false);
          write_all(fd, std::string_view(
              reinterpret_cast<const char*>(close_bytes.data()),
              close_bytes.size()));
          buf.clear();
          co_return;
        }

        // 데이터 프레임 → 애플리케이션 콜백
        if (on_message_) {
          co_await on_message_(std::move(frame));
        }
      }

      // 소비된 바이트 제거
      if (consumed > 0) {
        buf.erase(buf.begin(),
                  buf.begin() + static_cast<std::ptrdiff_t>(consumed));
      }
    }
    co_return;
  }

  // ── 프레임 전송 헬퍼 ────────────────────────────────────────────────────

  /**
   * @brief UTF-8 텍스트 프레임을 전송합니다.
   *
   * @param fd   대상 소켓 파일 디스크립터.
   * @param text 전송할 UTF-8 텍스트.
   * @returns 성공 시 `Result<void>::ok()`, 전송 실패 시 에러 코드.
   */
  Task<Result<void>> send_text(int fd, std::string_view text) {
    WsFrame frame;
    frame.opcode  = WsFrame::Opcode::Text;
    frame.fin     = true;
    frame.masked  = false;
    frame.payload.assign(
        reinterpret_cast<const uint8_t*>(text.data()),
        reinterpret_cast<const uint8_t*>(text.data()) + text.size());

    auto bytes = encode_frame(frame, false);
    if (!write_all(fd, std::string_view(
            reinterpret_cast<const char*>(bytes.data()), bytes.size()))) {
      co_return unexpected(
          std::error_code(errno, std::system_category()));
    }
    co_return Result<void>::ok();
  }

  /**
   * @brief 바이너리 프레임을 전송합니다.
   *
   * @param fd   대상 소켓 파일 디스크립터.
   * @param data 전송할 원시 바이트.
   * @returns 성공 시 `Result<void>::ok()`, 전송 실패 시 에러 코드.
   */
  Task<Result<void>> send_binary(int fd, std::span<const uint8_t> data) {
    WsFrame frame;
    frame.opcode  = WsFrame::Opcode::Binary;
    frame.fin     = true;
    frame.masked  = false;
    frame.payload.assign(data.begin(), data.end());

    auto bytes = encode_frame(frame, false);
    if (!write_all(fd, std::string_view(
            reinterpret_cast<const char*>(bytes.data()), bytes.size()))) {
      co_return unexpected(
          std::error_code(errno, std::system_category()));
    }
    co_return Result<void>::ok();
  }

  /**
   * @brief Close 프레임을 전송하고 연결 종료를 요청합니다.
   *
   * @param fd   대상 소켓 파일 디스크립터.
   * @param code RFC 6455 §7.4.1 정의 상태 코드. 기본값 1000 = 정상 종료.
   * @returns 성공 시 `Result<void>::ok()`, 전송 실패 시 에러 코드.
   */
  Task<Result<void>> send_close(int fd, uint16_t code = 1000) {
    WsFrame frame;
    frame.opcode = WsFrame::Opcode::Close;
    frame.fin    = true;
    frame.masked = false;
    // Close 프레임 페이로드: 2바이트 빅엔디언 상태 코드
    frame.payload.push_back(static_cast<uint8_t>(code >> 8));
    frame.payload.push_back(static_cast<uint8_t>(code & 0xFF));

    auto bytes = encode_frame(frame, false);
    if (!write_all(fd, std::string_view(
            reinterpret_cast<const char*>(bytes.data()), bytes.size()))) {
      co_return unexpected(
          std::error_code(errno, std::system_category()));
    }
    co_return Result<void>::ok();
  }

  /**
   * @brief Ping 프레임을 전송합니다.
   *
   * @param fd 대상 소켓 파일 디스크립터.
   * @returns 성공 시 `Result<void>::ok()`, 전송 실패 시 에러 코드.
   */
  Task<Result<void>> send_ping(int fd) {
    WsFrame frame;
    frame.opcode = WsFrame::Opcode::Ping;
    frame.fin    = true;
    frame.masked = false;

    auto bytes = encode_frame(frame, false);
    if (!write_all(fd, std::string_view(
            reinterpret_cast<const char*>(bytes.data()), bytes.size()))) {
      co_return unexpected(
          std::error_code(errno, std::system_category()));
    }
    co_return Result<void>::ok();
  }

private:
  // ── 프레임 인코딩/디코딩 ────────────────────────────────────────────────

  /**
   * @brief WsFrame을 RFC 6455 §5.2 형식의 바이트 시퀀스로 인코딩합니다.
   *
   * 서버 → 클라이언트 방향은 마스킹 없음(mask == false).
   * 클라이언트 → 서버 방향이 필요한 경우 mask == true를 사용합니다.
   *
   * @param frame 인코딩할 프레임.
   * @param mask  페이로드 마스킹 여부.
   * @returns 인코딩된 WebSocket 프레임 바이트.
   */
  static std::vector<uint8_t> encode_frame(const WsFrame& frame,
                                            bool mask = false) {
    std::vector<uint8_t> out;
    out.reserve(10 + frame.payload.size());

    // 바이트 0: FIN(1비트) + RSV(3비트) + Opcode(4비트)
    uint8_t b0 = static_cast<uint8_t>(
        (frame.fin ? 0x80u : 0x00u) |
        (static_cast<uint8_t>(frame.opcode) & 0x0Fu));
    out.push_back(b0);

    // 바이트 1: MASK(1비트) + Payload Len(7비트)
    uint64_t payload_len = frame.payload.size();
    uint8_t mask_bit = mask ? 0x80u : 0x00u;

    if (payload_len < 126) {
      out.push_back(mask_bit | static_cast<uint8_t>(payload_len));
    } else if (payload_len <= 0xFFFF) {
      out.push_back(mask_bit | 126u);
      out.push_back(static_cast<uint8_t>(payload_len >> 8));
      out.push_back(static_cast<uint8_t>(payload_len & 0xFF));
    } else {
      out.push_back(mask_bit | 127u);
      for (int i = 7; i >= 0; --i) {
        out.push_back(static_cast<uint8_t>((payload_len >> (i * 8)) & 0xFF));
      }
    }

    if (mask) {
      // 마스킹 키 (테스트용 고정값; 실제 사용 시 난수로 교체)
      std::array<uint8_t, 4> masking_key = {0x37, 0xfa, 0x21, 0x3d};
      out.insert(out.end(), masking_key.begin(), masking_key.end());
      for (size_t i = 0; i < frame.payload.size(); ++i) {
        out.push_back(frame.payload[i] ^ masking_key[i % 4]);
      }
    } else {
      out.insert(out.end(), frame.payload.begin(), frame.payload.end());
    }

    return out;
  }

  /**
   * @brief 바이트 스팬에서 WebSocket 프레임을 디코딩합니다.
   *
   * RFC 6455 §5.2에 따라 파싱합니다.
   * 데이터가 부족하면 `consumed`를 0으로 설정하고 `has_value() == false`인 결과를 반환합니다.
   *
   * @param data     디코딩할 바이트 범위.
   * @param consumed 성공 시 소비한 바이트 수.
   * @returns 디코딩된 프레임. 데이터 부족 시 has_value() == false.
   */
  static Result<WsFrame> decode_frame(std::span<const uint8_t> data,
                                       size_t& consumed) {
    consumed = 0;

    // 최소 2바이트 (헤더)
    if (data.size() < 2) {
      return unexpected(
          std::make_error_code(std::errc::resource_unavailable_try_again));
    }

    WsFrame frame;
    size_t pos = 0;

    uint8_t b0 = data[pos++];
    uint8_t b1 = data[pos++];

    frame.fin     = (b0 & 0x80u) != 0;
    frame.opcode  = static_cast<WsFrame::Opcode>(b0 & 0x0Fu);
    frame.masked  = (b1 & 0x80u) != 0;

    uint64_t payload_len = b1 & 0x7Fu;

    if (payload_len == 126) {
      if (data.size() < pos + 2) {
        return unexpected(
            std::make_error_code(std::errc::resource_unavailable_try_again));
      }
      payload_len = (static_cast<uint64_t>(data[pos]) << 8) |
                     static_cast<uint64_t>(data[pos + 1]);
      pos += 2;
    } else if (payload_len == 127) {
      if (data.size() < pos + 8) {
        return unexpected(
            std::make_error_code(std::errc::resource_unavailable_try_again));
      }
      payload_len = 0;
      for (int i = 0; i < 8; ++i) {
        payload_len = (payload_len << 8) | static_cast<uint64_t>(data[pos++]);
      }
    }

    // 마스킹 키 읽기 (클라이언트 → 서버는 항상 마스킹됨)
    std::array<uint8_t, 4> masking_key{};
    if (frame.masked) {
      if (data.size() < pos + 4) {
        return unexpected(
            std::make_error_code(std::errc::resource_unavailable_try_again));
      }
      masking_key[0] = data[pos++];
      masking_key[1] = data[pos++];
      masking_key[2] = data[pos++];
      masking_key[3] = data[pos++];
    }

    // 페이로드 읽기
    if (data.size() < pos + payload_len) {
      return unexpected(
          std::make_error_code(std::errc::resource_unavailable_try_again));
    }

    frame.payload.resize(static_cast<size_t>(payload_len));
    for (size_t i = 0; i < static_cast<size_t>(payload_len); ++i) {
      frame.payload[i] = data[pos + i];
      if (frame.masked) {
        frame.payload[i] ^= masking_key[i % 4];
      }
    }
    pos += static_cast<size_t>(payload_len);

    consumed = pos;
    return frame;
  }

  // ── SHA-1 + Base64 (Sec-WebSocket-Accept 계산) ──────────────────────────

  /**
   * @brief RFC 6455 §4.2.2에 따라 `Sec-WebSocket-Accept` 헤더 값을 계산합니다.
   *
   * `key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"` 에 SHA-1을 적용하고
   * Base64로 인코딩합니다.
   *
   * SHA-1 구현은 RFC 3174를 따르며 외부 의존성이 없습니다.
   * 이 함수는 보안 암호화 목적이 아닌 WebSocket 핸드셰이크 전용입니다.
   *
   * @param key `Sec-WebSocket-Key` 헤더 값.
   * @returns Base64 인코딩된 `Sec-WebSocket-Accept` 값.
   */
  static std::string compute_accept_key(std::string_view key) {
    static constexpr std::string_view kMagic =
        "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    std::string input(key);
    input.append(kMagic);

    // ── 인라인 SHA-1 (RFC 3174) ──────────────────────────────────────────
    auto rotl32 = [](uint32_t v, uint32_t n) -> uint32_t {
      return (v << n) | (v >> (32u - n));
    };

    std::array<uint32_t, 5> h = {
        0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};

    // メッセージ パディング
    std::vector<uint8_t> msg(input.begin(), input.end());
    uint64_t bit_len = static_cast<uint64_t>(msg.size()) * 8u;
    msg.push_back(0x80u);
    while (msg.size() % 64 != 56) msg.push_back(0x00u);
    for (int i = 7; i >= 0; --i) {
      msg.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFFu));
    }

    // ブロック処理
    for (size_t offset = 0; offset < msg.size(); offset += 64) {
      std::array<uint32_t, 80> w{};
      for (int i = 0; i < 16; ++i) {
        w[static_cast<size_t>(i)] =
            (static_cast<uint32_t>(msg[offset + static_cast<size_t>(i) * 4    ]) << 24u) |
            (static_cast<uint32_t>(msg[offset + static_cast<size_t>(i) * 4 + 1]) << 16u) |
            (static_cast<uint32_t>(msg[offset + static_cast<size_t>(i) * 4 + 2]) <<  8u) |
            (static_cast<uint32_t>(msg[offset + static_cast<size_t>(i) * 4 + 3]));
      }
      for (int i = 16; i < 80; ++i) {
        size_t si = static_cast<size_t>(i);
        w[si] = rotl32(w[si-3] ^ w[si-8] ^ w[si-14] ^ w[si-16], 1);
      }

      uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
      for (int i = 0; i < 80; ++i) {
        size_t si = static_cast<size_t>(i);
        uint32_t f, k;
        if (i < 20) {
          f = (b & c) | (~b & d);
          k = 0x5A827999u;
        } else if (i < 40) {
          f = b ^ c ^ d;
          k = 0x6ED9EBA1u;
        } else if (i < 60) {
          f = (b & c) | (b & d) | (c & d);
          k = 0x8F1BBCDCu;
        } else {
          f = b ^ c ^ d;
          k = 0xCA62C1D6u;
        }
        uint32_t temp = rotl32(a, 5) + f + e + k + w[si];
        e = d; d = c;
        c = rotl32(b, 30);
        b = a; a = temp;
      }
      h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    // SHA-1 다이제스트 (20바이트)
    std::array<uint8_t, 20> digest{};
    for (int i = 0; i < 5; ++i) {
      size_t si = static_cast<size_t>(i);
      digest[si * 4    ] = static_cast<uint8_t>(h[si] >> 24u);
      digest[si * 4 + 1] = static_cast<uint8_t>(h[si] >> 16u);
      digest[si * 4 + 2] = static_cast<uint8_t>(h[si] >>  8u);
      digest[si * 4 + 3] = static_cast<uint8_t>(h[si]);
    }

    // ── Base64 인코딩 ────────────────────────────────────────────────────
    static constexpr std::string_view kBase64Chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string encoded;
    encoded.reserve(28); // ceil(20/3)*4 = 28

    for (size_t i = 0; i < 20; i += 3) {
      uint32_t triple = static_cast<uint32_t>(digest[i]) << 16u;
      if (i + 1 < 20) triple |= static_cast<uint32_t>(digest[i + 1]) << 8u;
      if (i + 2 < 20) triple |= static_cast<uint32_t>(digest[i + 2]);

      encoded.push_back(kBase64Chars[(triple >> 18u) & 0x3Fu]);
      encoded.push_back(kBase64Chars[(triple >> 12u) & 0x3Fu]);
      encoded.push_back((i + 1 < 20) ? kBase64Chars[(triple >>  6u) & 0x3Fu] : '=');
      encoded.push_back((i + 2 < 20) ? kBase64Chars[(triple       ) & 0x3Fu] : '=');
    }

    return encoded;
  }

  // ── 소켓 I/O 헬퍼 ───────────────────────────────────────────────────────

  /**
   * @brief 소켓에 데이터를 모두 전송할 때까지 반복 write합니다.
   *
   * @param fd   대상 소켓 파일 디스크립터.
   * @param data 전송할 데이터.
   * @returns 성공 시 true, 에러 시 false.
   */
  static bool write_all(int fd, std::string_view data) noexcept {
    const char *ptr      = data.data();
    size_t      remaining = data.size();
    while (remaining > 0) {
      ssize_t n = ::write(fd, ptr, remaining);
      if (n <= 0) return false;
      ptr       += static_cast<size_t>(n);
      remaining -= static_cast<size_t>(n);
    }
    return true;
  }

  /** @brief 애플리케이션이 제공하는 수신 프레임 핸들러. */
  MessageHandler on_message_;
};

} // namespace qbuem

/** @} */ // end of qbuem_websocket_handler
