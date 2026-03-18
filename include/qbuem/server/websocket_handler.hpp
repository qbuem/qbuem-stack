#pragma once

/**
 * @file qbuem/server/websocket_handler.hpp
 * @brief RFC 6455 WebSocket handler — HTTP/1.1 upgrade handshake and frame processing
 * @defgroup qbuem_websocket_handler WebSocket Handler
 * @ingroup qbuem_server
 *
 * This header provides the core implementation of the RFC 6455 WebSocket protocol.
 *
 * ### Key features
 * - **HTTP/1.1 upgrade handshake**: Generates `101 Switching Protocols` response.
 * - **`Sec-WebSocket-Accept` computation**: SHA-1 + Base64 key derivation per RFC 6455 §4.2.2.
 * - **Frame encoding/decoding**: Variable-length payload, masking support (RFC 6455 §5.3).
 * - **Control frames**: Automatic Ping/Pong/Close handling.
 * - **Message handler injection**: Delivers received frames to the application via a constructor callback.
 *
 * ### SHA-1 note
 * SHA-1 is required for the `Sec-WebSocket-Accept` computation in the WebSocket handshake.
 * This implementation includes an inline SHA-1 based on RFC 3174 with no external dependencies,
 * used exclusively for the handshake and not for TLS security purposes.
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
 * @brief RFC 6455 WebSocket frame.
 *
 * The basic data unit of the WebSocket protocol.
 * Frames decoded by `WebSocketHandler` are delivered to the application callback.
 *
 * ### Multi-frame messages (fragmentation)
 * - First frame: `fin == false`, opcode == Text or Binary.
 * - Intermediate frames: `fin == false`, opcode == Continuation.
 * - Last frame: `fin == true`, opcode == Continuation.
 */
struct WsFrame {
  /**
   * @brief WebSocket frame opcode (RFC 6455 §5.2).
   */
  enum class Opcode : uint8_t {
    Continuation = 0x0, ///< Continuation of a previous frame (fragmentation)
    Text         = 0x1, ///< UTF-8 text data
    Binary       = 0x2, ///< Arbitrary binary data
    Close        = 0x8, ///< Connection close request
    Ping         = 0x9, ///< Ping (keepalive check)
    Pong         = 0xA, ///< Response to a Ping
  };

  /** @brief Opcode indicating the frame type. */
  Opcode opcode{Opcode::Text};

  /** @brief Whether this is the final frame. false means it is part of a multi-frame message. */
  bool fin{true};

  /** @brief Whether the payload is masked. Must be true for client → server direction. */
  bool masked{false};

  /** @brief Frame payload data. Already unmasked if masking was applied. */
  std::vector<uint8_t> payload;
};

// ─── WebSocketHandler ─────────────────────────────────────────────────────────

/**
 * @brief RFC 6455 WebSocket server handler.
 *
 * Handles server-side WebSocket protocol processing from the HTTP/1.1 Upgrade
 * handshake through binary/text frame send and receive.
 *
 * ### Usage example
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
 * // Connect as the upgrade callback of Http1Handler
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
   * @brief Callback type for handling received WebSocket frames.
   *
   * Called by the `run()` loop each time a complete frame is received.
   * Control frames (Ping, Close) are handled automatically and not passed to the callback.
   */
  using MessageHandler = std::function<Task<void>(WsFrame)>;

  /**
   * @brief Constructs a WebSocketHandler.
   *
   * @param on_message Callback invoked for each received frame.
   */
  explicit WebSocketHandler(MessageHandler on_message)
      : on_message_(std::move(on_message)) {}

  // ── Upgrade handshake ───────────────────────────────────────────────────

  /**
   * @brief Performs the HTTP/1.1 WebSocket upgrade handshake.
   *
   * RFC 6455 §4.2.2 procedure:
   * 1. Validate `Sec-WebSocket-Key` header.
   * 2. Compute `Sec-WebSocket-Accept` key (SHA-1 + Base64).
   * 3. Send `101 Switching Protocols` response.
   *
   * @param fd  Connected socket file descriptor.
   * @param req Original HTTP request that initiated the upgrade.
   * @returns `Result<void>::ok()` on success, or an error code for an invalid request.
   */
  Task<Result<void>> upgrade(int fd, const Request& req) {
    std::string_view ws_key = req.header("Sec-WebSocket-Key");
    if (ws_key.empty()) {
      // 400 Bad Request: Sec-WebSocket-Key missing
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

  // ── Frame send/receive loop ─────────────────────────────────────────────

  /**
   * @brief Runs the WebSocket frame receive/dispatch loop.
   *
   * Call after a successful `upgrade()`. Repeats the following until the connection closes:
   * - Read frames from the socket.
   * - Ping → automatic Pong response.
   * - Close → echo Close and exit loop.
   * - Text/Binary/Continuation → invoke `on_message_` callback.
   *
   * @param fd Connected socket file descriptor.
   */
  Task<void> run(int fd) {
    std::vector<uint8_t> buf;
    buf.reserve(4096);

    for (;;) {
      // Receive data from socket (simplified: up to 64KB chunks)
      uint8_t tmp[65536];
      ssize_t n = ::read(fd, tmp, sizeof(tmp));
      if (n <= 0) break; // EOF or error

      buf.insert(buf.end(), tmp, tmp + n);

      // Extract complete frames from buffer
      size_t consumed = 0;
      for (;;) {
        auto span = std::span<const uint8_t>(buf.data() + consumed,
                                             buf.size() - consumed);
        size_t frame_consumed = 0;
        auto result = decode_frame(span, frame_consumed);
        if (!result.has_value()) break; // Frame incomplete

        WsFrame frame = std::move(result.value());
        consumed += frame_consumed;

        // Automatically handle control frames
        if (frame.opcode == WsFrame::Opcode::Ping) {
          // Send Pong response
          auto pong_bytes = encode_frame(
              WsFrame{WsFrame::Opcode::Pong, true, false, {}}, false);
          write_all(fd, std::string_view(
              reinterpret_cast<const char*>(pong_bytes.data()),
              pong_bytes.size()));
          continue;
        }

        if (frame.opcode == WsFrame::Opcode::Close) {
          // Echo Close and exit loop
          auto close_bytes = encode_frame(
              WsFrame{WsFrame::Opcode::Close, true, false,
                      frame.payload}, false);
          write_all(fd, std::string_view(
              reinterpret_cast<const char*>(close_bytes.data()),
              close_bytes.size()));
          buf.clear();
          co_return;
        }

        // Data frame → application callback
        if (on_message_) {
          co_await on_message_(std::move(frame));
        }
      }

      // Remove consumed bytes
      if (consumed > 0) {
        buf.erase(buf.begin(),
                  buf.begin() + static_cast<std::ptrdiff_t>(consumed));
      }
    }
    co_return;
  }

  // ── Frame send helpers ───────────────────────────────────────────────────

  /**
   * @brief Sends a UTF-8 text frame.
   *
   * @param fd   Target socket file descriptor.
   * @param text UTF-8 text to send.
   * @returns `Result<void>::ok()` on success, or an error code on send failure.
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
   * @brief Sends a binary frame.
   *
   * @param fd   Target socket file descriptor.
   * @param data Raw bytes to send.
   * @returns `Result<void>::ok()` on success, or an error code on send failure.
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
   * @brief Sends a Close frame and requests connection closure.
   *
   * @param fd   Target socket file descriptor.
   * @param code Status code defined in RFC 6455 §7.4.1. Default 1000 = normal closure.
   * @returns `Result<void>::ok()` on success, or an error code on send failure.
   */
  Task<Result<void>> send_close(int fd, uint16_t code = 1000) {
    WsFrame frame;
    frame.opcode = WsFrame::Opcode::Close;
    frame.fin    = true;
    frame.masked = false;
    // Close frame payload: 2-byte big-endian status code
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
   * @brief Sends a Ping frame.
   *
   * @param fd Target socket file descriptor.
   * @returns `Result<void>::ok()` on success, or an error code on send failure.
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

  // ── Static utilities (public for testing and examples) ──────────────────

  /**
   * @brief Encodes a WsFrame as a byte sequence in RFC 6455 §5.2 format.
   *
   * Server → client direction uses no masking (mask == false).
   * Use mask == true when client → server direction is required.
   *
   * @param frame Frame to encode.
   * @param mask  Whether to mask the payload.
   * @returns Encoded WebSocket frame bytes.
   */
  static std::vector<uint8_t> encode_frame(const WsFrame& frame,
                                            bool mask = false) {
    std::vector<uint8_t> out;
    out.reserve(10 + frame.payload.size());

    // Byte 0: FIN(1 bit) + RSV(3 bits) + Opcode(4 bits)
    uint8_t b0 = static_cast<uint8_t>(
        (frame.fin ? 0x80u : 0x00u) |
        (static_cast<uint8_t>(frame.opcode) & 0x0Fu));
    out.push_back(b0);

    // Byte 1: MASK(1 bit) + Payload Len(7 bits)
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
      // Masking key (fixed value for testing; replace with random value in production)
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
   * @brief Decodes a WebSocket frame from a byte span.
   *
   * Parses according to RFC 6455 §5.2.
   * Sets `consumed` to 0 and returns a result with `has_value() == false` if data is insufficient.
   *
   * @param data     Byte range to decode.
   * @param consumed Number of bytes consumed on success.
   * @returns Decoded frame. has_value() == false if data is insufficient.
   */
  static Result<WsFrame> decode_frame(std::span<const uint8_t> data,
                                       size_t& consumed) {
    consumed = 0;

    // Minimum 2 bytes (header)
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

    // Read masking key (client → server is always masked)
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

    // Read payload
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

  // ── SHA-1 + Base64 (Sec-WebSocket-Accept computation) ───────────────────

  /**
   * @brief Computes the `Sec-WebSocket-Accept` header value per RFC 6455 §4.2.2.
   *
   * Applies SHA-1 to `key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"`
   * and encodes the result in Base64.
   *
   * The SHA-1 implementation follows RFC 3174 with no external dependencies.
   * This function is for WebSocket handshake purposes only, not for security cryptography.
   *
   * @param key `Sec-WebSocket-Key` header value.
   * @returns Base64-encoded `Sec-WebSocket-Accept` value.
   */
  static std::string compute_accept_key(std::string_view key) {
    static constexpr std::string_view kMagic =
        "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    std::string input(key);
    input.append(kMagic);

    // ── Inline SHA-1 (RFC 3174) ──────────────────────────────────────────
    auto rotl32 = [](uint32_t v, uint32_t n) -> uint32_t {
      return (v << n) | (v >> (32u - n));
    };

    std::array<uint32_t, 5> h = {
        0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};

    // Message padding
    std::vector<uint8_t> msg(input.begin(), input.end());
    uint64_t bit_len = static_cast<uint64_t>(msg.size()) * 8u;
    msg.push_back(0x80u);
    while (msg.size() % 64 != 56) msg.push_back(0x00u);
    for (int i = 7; i >= 0; --i) {
      msg.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFFu));
    }

    // Block processing
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

    // SHA-1 digest (20 bytes)
    std::array<uint8_t, 20> digest{};
    for (int i = 0; i < 5; ++i) {
      size_t si = static_cast<size_t>(i);
      digest[si * 4    ] = static_cast<uint8_t>(h[si] >> 24u);
      digest[si * 4 + 1] = static_cast<uint8_t>(h[si] >> 16u);
      digest[si * 4 + 2] = static_cast<uint8_t>(h[si] >>  8u);
      digest[si * 4 + 3] = static_cast<uint8_t>(h[si]);
    }

    // ── Base64 encoding ──────────────────────────────────────────────────
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

  // ── Socket I/O helpers ───────────────────────────────────────────────────

  /**
   * @brief Repeatedly writes to a socket until all data is sent.
   *
   * @param fd   Target socket file descriptor.
   * @param data Data to send.
   * @returns true on success, false on error.
   */
private:
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

  /** @brief Received frame handler provided by the application. */
  MessageHandler on_message_;
};

} // namespace qbuem

/** @} */ // end of qbuem_websocket_handler
