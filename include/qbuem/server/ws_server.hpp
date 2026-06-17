#pragma once

/**
 * @file qbuem/server/ws_server.hpp
 * @brief High-level, non-blocking, multi-connection WebSocket server (RFC 6455)
 * @defgroup qbuem_ws_server WebSocket Server
 * @ingroup qbuem_server
 *
 * `WsServer<Ctx>` is the production, high-concurrency WebSocket layer. Unlike
 * the low-level @ref qbuem::WebSocketHandler (single connection, blocking
 * `::read`/`::write`), this server is fully **reactor-driven and
 * non-blocking**, so one slow peer never stalls the event loop, and a single
 * reactor thread can drive tens of thousands of connections.
 *
 * ### Feature set
 * - **Non-blocking I/O**: persistent reactor Read callback + buffered,
 *   back-pressured Write callback (no blocking syscalls on the reactor thread).
 * - **Lifecycle callbacks**: `on_open` / `on_message` / `on_close` / `on_error`.
 * - **Per-connection context** `Ctx`: attach arbitrary application state to
 *   each connection (e.g. a player id) with zero extra allocation.
 * - **Connection registry + rooms + broadcast**: O(1) lookup by id, named
 *   rooms, `broadcast_*()` / `broadcast_room_*()`.
 * - **Back-pressure**: bounded per-connection send queue. A peer that cannot
 *   keep up is dropped (closed) rather than growing server memory without
 *   bound — the correct policy for a real-time game server.
 * - **Heartbeat**: periodic Ping with a Pong deadline; dead peers are detected
 *   and closed within `pong_timeout_ms`.
 * - **Strict RFC 6455 framing**: fragmentation reassembly, control-frame
 *   constraints (FIN + ≤125 bytes), RSV/opcode/mask validation, message-size
 *   cap, optional UTF-8 validation for text, and a proper Close handshake.
 * - **Zero-allocation send fast path**: header is encoded into a stack buffer
 *   and sent with the payload in a single `::writev` (scatter-gather); the
 *   send buffer is only touched under back-pressure.
 *
 * ### Threading model (single-reactor-per-server)
 * A `WsServer` instance and all of its connections live on **one reactor
 * thread** — the registry and rooms are not locked. This is the
 * shared-nothing model used by Redis / Node.js and is ideal for an
 * authoritative game world (one match = one thread = one `WsServer`). To scale
 * across cores, run one `WsServer` per reactor thread behind `SO_REUSEPORT`
 * (the kernel load-balances accepts). `WsConnection::send_*()` is nonetheless
 * safe to call from another thread: it detects the foreign thread and forwards
 * the work to the owning reactor via `Reactor::post()`.
 *
 * ### Usage
 * @code
 * struct Player { uint32_t id; std::string name; };
 *
 * qbuem::WsServer<Player> server({
 *   .on_open    = [](auto conn){ conn->context().id = next_player_id(); },
 *   .on_message = [&](auto conn, qbuem::WsMessage msg){
 *       apply_input(conn->context(), msg.bytes());      // synchronous, fast
 *   },
 *   .on_close   = [](auto conn, uint16_t code){ remove_player(conn->context()); },
 * });
 *
 * // Inside a Dispatcher worker coroutine:
 * co_await server.listen(9001);            // bind + accept + upgrade + run
 *
 * // From the game tick (same reactor thread):
 * server.broadcast_binary(snapshot_bytes);
 * @endcode
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/awaiters.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/http/parser.hpp>
#include <qbuem/http/request.hpp>
#include <qbuem/io/iovec.hpp>
#include <qbuem/net/socket_addr.hpp>
#include <qbuem/server/http1_handler.hpp>
#include <qbuem/server/websocket_handler.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace qbuem {

// ─── WsCloseCode ─────────────────────────────────────────────────────────────

/**
 * @brief RFC 6455 §7.4.1 WebSocket close status codes.
 */
enum class WsCloseCode : uint16_t {
  Normal          = 1000, ///< Normal closure.
  GoingAway       = 1001, ///< Endpoint is going away (server shutdown, dead peer).
  ProtocolError   = 1002, ///< Protocol violation.
  UnsupportedData = 1003, ///< Received data of a type it cannot accept.
  InvalidPayload  = 1007, ///< Text payload was not valid UTF-8.
  PolicyViolation = 1008, ///< Generic policy violation.
  MessageTooBig   = 1009, ///< Message exceeded the configured size limit.
  InternalError   = 1011, ///< Server encountered an unexpected condition.
  TryAgainLater   = 1013, ///< Server is overloaded / back-pressure drop.
};

/** @brief WS close code used internally for abnormal teardown (never sent on the wire). */
inline constexpr uint16_t kWsAbnormalClosure = 1006;

// ─── UTF-8 validation (RFC 6455 §8.1) ────────────────────────────────────────

/**
 * @brief Strict UTF-8 well-formedness check.
 *
 * Rejects overlong encodings, UTF-16 surrogate code points (U+D800..U+DFFF),
 * values above U+10FFFF, and truncated multi-byte sequences. Used by `WsServer`
 * to enforce that Text frames carry valid UTF-8 (closing with code 1007
 * otherwise). Exposed as a free function so it is unit-testable and shared
 * across all `WsServer<Ctx>` instantiations.
 *
 * @param s   Pointer to the bytes to validate.
 * @param len Number of bytes.
 * @returns true if `[s, s+len)` is well-formed UTF-8.
 */
[[nodiscard]] inline bool ws_is_valid_utf8(const uint8_t* s, size_t len) noexcept {
  size_t i = 0;
  while (i < len) {
    const uint8_t c = s[i];
    if (c < 0x80) { ++i; continue; }
    size_t extra;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) {
      if (c < 0xC2) return false; // overlong 2-byte
      extra = 1; cp = c & 0x1Fu;
    } else if ((c & 0xF0) == 0xE0) {
      extra = 2; cp = c & 0x0Fu;
    } else if ((c & 0xF8) == 0xF0) {
      if (c > 0xF4) return false; // > U+10FFFF
      extra = 3; cp = c & 0x07u;
    } else {
      return false; // lone continuation byte, or 0xF5..0xFF
    }
    if (i + extra >= len) return false; // truncated sequence
    for (size_t k = 1; k <= extra; ++k) {
      const uint8_t cc = s[i + k];
      if ((cc & 0xC0u) != 0x80u) return false;
      cp = (cp << 6) | (cc & 0x3Fu);
    }
    if (extra == 2 && cp < 0x800) return false;     // overlong 3-byte
    if (extra == 3 && cp < 0x10000) return false;   // overlong 4-byte
    if (cp >= 0xD800 && cp <= 0xDFFF) return false; // UTF-16 surrogate
    if (cp > 0x10FFFF) return false;
    i += extra + 1;
  }
  return true;
}

// ─── WsMessage ───────────────────────────────────────────────────────────────

/**
 * @brief A complete, reassembled application message delivered to `on_message`.
 *
 * The view is valid only for the duration of the `on_message` callback. Copy
 * the bytes if you need to retain them past the callback.
 */
struct WsMessage {
  /** @brief true for a Text message, false for Binary. */
  bool is_text{false};
  /** @brief The full message payload (fragments already reassembled). */
  std::span<const std::byte> data;

  /** @brief View the payload as raw bytes. */
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return data; }

  /**
   * @brief View a text message as a `std::string_view`.
   * @note Only meaningful when `is_text` is true.
   */
  [[nodiscard]] std::string_view text() const noexcept {
    return {reinterpret_cast<const char*>(data.data()), data.size()};
  }
};

// ─── WsServerConfig ──────────────────────────────────────────────────────────

/**
 * @brief Tunable limits and timeouts for a `WsServer`.
 */
struct WsServerConfig {
  /** @brief Maximum reassembled message size. Larger → Close 1009. */
  size_t max_message_size = 16ull * 1024 * 1024;
  /** @brief Maximum bytes buffered for a single connection's send queue.
   *         Exceeding it drops (closes) the slow connection. */
  size_t max_send_queue = 8ull * 1024 * 1024;
  /** @brief Interval between heartbeat Pings (ms). 0 disables heartbeat. */
  int ping_interval_ms = 30000;
  /** @brief Time to wait for a Pong after a Ping before declaring the peer dead (ms). */
  int pong_timeout_ms = 10000;
  /** @brief Maximum number of simultaneous connections. Beyond it → 503. */
  size_t max_connections = 65536;
  /** @brief Validate that Text payloads are well-formed UTF-8 (RFC 6455 §8.1). */
  bool validate_utf8 = true;
  /** @brief Set TCP_NODELAY on accepted sockets (low latency, recommended for games). */
  bool tcp_nodelay = true;
  /** @brief Time a connection has to complete the HTTP upgrade handshake before
   *  it is dropped (ms). Bounds slowloris on the pre-upgrade reader, which is
   *  not yet covered by the post-upgrade heartbeat. 0 disables. Used by listen(). */
  int handshake_timeout_ms = 10000;
};

// Forward declaration — WsConnection holds a back-pointer to its owning server.
template <class Ctx> class WsServer;

// ─── WsConnection<Ctx> ───────────────────────────────────────────────────────

/**
 * @brief A single live WebSocket connection, owned by a `WsServer`.
 *
 * Drives one socket non-blockingly: a persistent reactor Read callback decodes
 * inbound frames, and a buffered Write callback flushes outbound frames with
 * back-pressure. Application code interacts with it via `send_text()`,
 * `send_binary()`, `close()`, `context()`, and `id()`.
 *
 * @tparam Ctx Per-connection application state type.
 */
template <class Ctx = std::monostate>
class WsConnection : public std::enable_shared_from_this<WsConnection<Ctx>> {
public:
  /** @brief Private-construction tag — create connections only via `WsServer`. */
  struct PrivateTag { explicit PrivateTag() = default; };

  using Self = std::shared_ptr<WsConnection<Ctx>>;

  WsConnection(PrivateTag, uint64_t id, int fd, Reactor* reactor,
               WsServer<Ctx>* server, SocketAddr remote) noexcept
      : id_(id), fd_(fd), reactor_(reactor), server_(server),
        remote_(remote) {}

  WsConnection(const WsConnection&)            = delete;
  WsConnection& operator=(const WsConnection&) = delete;

  ~WsConnection() {
    if (fd_ >= 0) ::close(fd_);
  }

  // ── Accessors ─────────────────────────────────────────────────────────────

  /** @brief Unique, monotonically-assigned connection id. */
  [[nodiscard]] uint64_t id() const noexcept { return id_; }
  /** @brief Mutable per-connection application context. */
  [[nodiscard]] Ctx& context() noexcept { return ctx_; }
  /** @brief Const per-connection application context. */
  [[nodiscard]] const Ctx& context() const noexcept { return ctx_; }
  /** @brief Remote peer address (default-constructed if unavailable). */
  [[nodiscard]] SocketAddr remote() const noexcept { return remote_; }
  /** @brief Whether the connection is open (handshake done, not closing). */
  [[nodiscard]] bool is_open() const noexcept { return open_ && !closing_; }

  // ── Send API (safe to call from any thread) ────────────────────────────────

  /**
   * @brief Queue a UTF-8 text message for sending.
   * @returns false if the connection is closed or the send queue is full
   *          (the connection is then closed). When called from a foreign
   *          thread the work is posted to the owning reactor and true is
   *          returned optimistically.
   */
  bool send_text(std::string_view text) {
    return send_user(WsFrame::Opcode::Text,
                     std::as_bytes(std::span{text.data(), text.size()}));
  }

  /**
   * @brief Queue a binary message for sending.
   * @returns see `send_text()`.
   */
  bool send_binary(std::span<const std::byte> data) {
    return send_user(WsFrame::Opcode::Binary, data);
  }

  /**
   * @brief Begin a graceful close handshake (sends a Close frame, then tears down).
   * @param code   RFC 6455 close code.
   * @param reason Optional UTF-8 reason (truncated to fit a 125-byte control frame).
   */
  void close(WsCloseCode code = WsCloseCode::Normal, std::string_view reason = {}) {
    if (Reactor::current() != reactor_) {
      auto self = this->shared_from_this();
      uint16_t c = std::to_underlying(code);
      std::string r{reason};
      reactor_->post([self, c, r] { self->begin_close(c, r, self); });
      return;
    }
    begin_close(std::to_underlying(code), reason, this->shared_from_this());
  }

private:
  friend class WsServer<Ctx>;

  // ── Setup (called by WsServer) ──────────────────────────────────────────────

  // Queue the HTTP/1.1 101 handshake response (raw bytes, not a WS frame).
  void queue_handshake(std::string_view response, const Self& self) {
    raw_append(reinterpret_cast<const std::byte*>(response.data()),
               response.size());
    arm_write(self);
  }

  // Register the persistent Read callback and start the heartbeat timer.
  void start(const Self& self) {
    open_ = true;
    arm_read(self);
    arm_ping(self);
  }

  // ── Read path ───────────────────────────────────────────────────────────────

  void arm_read(const Self& self) {
    if (read_armed_ || finished_) return;
    read_armed_ = true;
    reactor_->register_event(fd_, EventType::Read,
                             [self](int fd) { self->on_readable(fd, self); });
  }

  // self by value: keeps the connection alive for the whole callback even if
  // teardown unregisters (and destroys) the stored callback mid-execution.
  void on_readable(int fd, Self self) {
    if (closing_ || finished_) return;

    static thread_local std::array<uint8_t, 65536> scratch;
    ssize_t n = ::read(fd, scratch.data(), scratch.size());
    if (n <= 0) {
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
      // EOF or hard error — abnormal teardown (no Close frame can be sent).
      report_error(std::error_code(n == 0 ? 0 : errno, std::system_category()),
                   self);
      finish_close(kWsAbnormalClosure, self);
      return;
    }

    rbuf_.insert(rbuf_.end(), scratch.data(), scratch.data() + n);
    drain_frames(self);
  }

  // Decode and dispatch every complete frame in rbuf_. Returns with the buffer
  // compacted; tears the connection down on protocol violations.
  void drain_frames(const Self& self) {
    size_t pos = 0;
    while (true) {
      size_t consumed = 0;
      auto r = WebSocketHandler::decode_frame(
          std::span<const uint8_t>(rbuf_.data() + pos, rbuf_.size() - pos),
          consumed);
      if (!r) {
        if (r.error() == std::make_error_code(std::errc::message_size)) {
          begin_close(std::to_underlying(WsCloseCode::MessageTooBig), {}, self);
          return; // connection torn down / closing
        }
        break; // incomplete frame — wait for more bytes
      }
      pos += consumed;
      if (!handle_frame(*r, self)) return;     // teardown started
      if (closing_ || finished_) return;
    }
    if (pos > 0)
      rbuf_.erase(rbuf_.begin(), rbuf_.begin() + static_cast<std::ptrdiff_t>(pos));
    // An incomplete-but-oversized accumulation cannot happen: decode_frame caps
    // the payload at 16 MiB and rejects larger lengths before buffering grows.
  }

  // Validate and act on one decoded frame. Returns false if teardown started.
  bool handle_frame(WsFrame& frame, const Self& self) {
    // RSV bits require a negotiated extension (none supported) → protocol error.
    if (frame.rsv1 || frame.rsv2 || frame.rsv3) {
      begin_close(std::to_underlying(WsCloseCode::ProtocolError), {}, self);
      return false;
    }
    // RFC 6455 §5.1: client-to-server frames MUST be masked.
    if (!frame.masked) {
      begin_close(std::to_underlying(WsCloseCode::ProtocolError), {}, self);
      return false;
    }

    const WsFrame::Opcode op = frame.opcode;
    const bool is_control = (op == WsFrame::Opcode::Close ||
                             op == WsFrame::Opcode::Ping ||
                             op == WsFrame::Opcode::Pong);
    const bool is_data = (op == WsFrame::Opcode::Continuation ||
                          op == WsFrame::Opcode::Text ||
                          op == WsFrame::Opcode::Binary);
    if (!is_control && !is_data) { // unknown opcode
      begin_close(std::to_underlying(WsCloseCode::ProtocolError), {}, self);
      return false;
    }
    // Control frames must not be fragmented and must be ≤125 bytes.
    if (is_control && (!frame.fin || frame.payload.size() > 125)) {
      begin_close(std::to_underlying(WsCloseCode::ProtocolError), {}, self);
      return false;
    }

    switch (op) {
      case WsFrame::Opcode::Ping:
        send_frame(WsFrame::Opcode::Pong,
                   std::as_bytes(std::span{frame.payload.data(),
                                           frame.payload.size()}),
                   &self);
        return !finished_;
      case WsFrame::Opcode::Pong:
        on_pong(self);
        return true;
      case WsFrame::Opcode::Close: {
        uint16_t code = std::to_underlying(WsCloseCode::Normal);
        if (frame.payload.size() >= 2)
          code = static_cast<uint16_t>((frame.payload[0] << 8) | frame.payload[1]);
        begin_close(code, {}, self);
        return false;
      }
      default:
        return handle_data(frame, self);
    }
  }

  // Fragmentation reassembly + message delivery.
  bool handle_data(WsFrame& frame, const Self& self) {
    const WsServerConfig& cfg = server_->config();
    if (frame.opcode == WsFrame::Opcode::Continuation) {
      if (frag_opcode_ == 0) { // continuation with no started message
        begin_close(std::to_underlying(WsCloseCode::ProtocolError), {}, self);
        return false;
      }
      if (assembly_.size() + frame.payload.size() > cfg.max_message_size) {
        begin_close(std::to_underlying(WsCloseCode::MessageTooBig), {}, self);
        return false;
      }
      assembly_.insert(assembly_.end(), frame.payload.begin(),
                       frame.payload.end());
      if (frame.fin) {
        const bool is_text = (frag_opcode_ ==
                              std::to_underlying(WsFrame::Opcode::Text));
        WsFrame::Opcode op = static_cast<WsFrame::Opcode>(frag_opcode_);
        frag_opcode_ = 0;
        bool ok = deliver(op, assembly_.data(), assembly_.size(), is_text, self);
        assembly_.clear();
        return ok;
      }
      return true;
    }

    // New Text/Binary data frame.
    if (frag_opcode_ != 0) { // a previous fragmented message is still open
      begin_close(std::to_underlying(WsCloseCode::ProtocolError), {}, self);
      return false;
    }
    if (frame.payload.size() > cfg.max_message_size) {
      begin_close(std::to_underlying(WsCloseCode::MessageTooBig), {}, self);
      return false;
    }
    const bool is_text = (frame.opcode == WsFrame::Opcode::Text);
    if (frame.fin) {
      return deliver(frame.opcode, frame.payload.data(), frame.payload.size(),
                     is_text, self);
    }
    // Start of a fragmented message.
    frag_opcode_ = std::to_underlying(frame.opcode);
    assembly_.assign(frame.payload.begin(), frame.payload.end());
    return true;
  }

  // Validate (UTF-8) and hand a complete message to the application.
  bool deliver(WsFrame::Opcode /*op*/, const uint8_t* data, size_t len,
               bool is_text, const Self& self) {
    if (is_text && server_->config().validate_utf8 &&
        !ws_is_valid_utf8(data, len)) {
      begin_close(std::to_underlying(WsCloseCode::InvalidPayload), {}, self);
      return false;
    }
    WsMessage msg{is_text, std::span<const std::byte>(
                               reinterpret_cast<const std::byte*>(data), len)};
    server_->dispatch_message(self, msg);
    return !closing_ && !finished_;
  }

  // ── Send / Write path ───────────────────────────────────────────────────────

  // Application send (Text/Binary). Blocked once the connection is closing so
  // no data frame can follow a queued Close. Thread-safe: a foreign-thread call
  // copies the payload and forwards it to the owning reactor.
  bool send_user(WsFrame::Opcode op, std::span<const std::byte> payload) {
    if (Reactor::current() == reactor_) {
      if (finished_ || closing_) return false;
      auto self = this->shared_from_this();
      return send_frame(op, payload, &self);
    }
    // Foreign thread: copy and post to the owning reactor.
    auto self = this->shared_from_this();
    auto buf = std::make_shared<std::vector<std::byte>>(payload.begin(),
                                                        payload.end());
    reactor_->post([self, op, buf] {
      if (self->finished_ || self->closing_) return;
      self->send_frame(op, std::span<const std::byte>(buf->data(), buf->size()),
                       &self);
    });
    return true;
  }

  // Encode and send one frame on the reactor thread. Used for both application
  // frames (via send_user) and control frames (Ping/Pong/Close) — the latter
  // must be sendable while `closing_`, so this only refuses when `finished_`.
  // `self` must be live for the duration. Returns false if the frame was
  // dropped (connection finished / back-pressure abort).
  bool send_frame(WsFrame::Opcode op, std::span<const std::byte> payload,
                  const Self* self) {
    if (finished_) return false;

    uint8_t hdr[WebSocketHandler::kMaxHeaderBytes];
    const size_t hlen = WebSocketHandler::encode_header(
        hdr, op, payload.size(), /*fin=*/true, /*mask=*/false);

    const size_t pending = sbuf_.size() - ssent_;
    const size_t need = hlen + payload.size();
    if (pending + need > server_->config().max_send_queue) {
      // Slow consumer — drop it rather than grow unbounded.
      report_error(std::make_error_code(std::errc::no_buffer_space), *self);
      finish_close(std::to_underlying(WsCloseCode::TryAgainLater), *self);
      return false;
    }

    if (pending == 0 && !write_armed_) {
      // Fast path: single writev of header + payload, no buffering.
      IOVec<2> vec;
      vec.push(hdr, hlen);
      if (!payload.empty()) vec.push(payload.data(), payload.size());
      ssize_t n = ::writev(fd_, vec.vecs.data(), static_cast<int>(vec.count));
      if (n >= 0) {
        if (static_cast<size_t>(n) == need) return true; // fully sent
        buffer_tail(hdr, hlen, payload, static_cast<size_t>(n));
        arm_write(*self);
        return true;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        raw_append(reinterpret_cast<const std::byte*>(hdr), hlen);
        raw_append(payload.data(), payload.size());
        arm_write(*self);
        return true;
      }
      report_error(std::error_code(errno, std::system_category()), *self);
      finish_close(kWsAbnormalClosure, *self);
      return false;
    }

    // Already buffering — append and keep the Write callback armed.
    raw_append(reinterpret_cast<const std::byte*>(hdr), hlen);
    raw_append(payload.data(), payload.size());
    if (!write_armed_) arm_write(*self);
    return true;
  }

  // Append the unsent tail after a partial writev (n bytes of header+payload sent).
  void buffer_tail(const uint8_t* hdr, size_t hlen,
                   std::span<const std::byte> payload, size_t sent) {
    if (sent < hlen) {
      raw_append(reinterpret_cast<const std::byte*>(hdr) + sent, hlen - sent);
      raw_append(payload.data(), payload.size());
    } else {
      const size_t po = sent - hlen;
      raw_append(payload.data() + po, payload.size() - po);
    }
  }

  void raw_append(const std::byte* p, size_t n) {
    if (n == 0) return;
    sbuf_.insert(sbuf_.end(), p, p + n);
  }

  void arm_write(const Self& self) {
    if (write_armed_ || finished_) return;
    write_armed_ = true;
    reactor_->register_event(fd_, EventType::Write,
                             [self](int fd) { self->on_writable(fd, self); });
  }

  void on_writable(int fd, Self self) {
    if (finished_) return;
    while (ssent_ < sbuf_.size()) {
      ssize_t n = ::write(fd, sbuf_.data() + ssent_, sbuf_.size() - ssent_);
      if (n > 0) {
        ssent_ += static_cast<size_t>(n);
      } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return; // stay armed; the reactor will call us again when writable
      } else {
        report_error(std::error_code(errno, std::system_category()), self);
        finish_close(kWsAbnormalClosure, self);
        return;
      }
    }
    // Fully drained.
    sbuf_.clear();
    ssent_ = 0;
    reactor_->unregister_event(fd_, EventType::Write);
    write_armed_ = false;
    if (closing_) finish_close(close_code_, self);
  }

  // ── Heartbeat ───────────────────────────────────────────────────────────────

  void arm_ping(const Self& self) {
    if (finished_ || closing_) return;
    if (server_->config().ping_interval_ms <= 0) return;
    auto r = reactor_->register_timer(
        server_->config().ping_interval_ms,
        [self](int) { self->on_ping_tick(self); });
    if (r) ping_timer_id_ = *r;
  }

  void on_ping_tick(Self self) {
    ping_timer_id_ = -1;
    if (finished_ || closing_) return;
    send_frame(WsFrame::Opcode::Ping, {}, &self);
    if (finished_ || closing_) return; // send may have aborted on back-pressure
    awaiting_pong_ = true;
    auto r = reactor_->register_timer(
        server_->config().pong_timeout_ms,
        [self](int) { self->on_pong_timeout(self); });
    if (r) pong_timer_id_ = *r;
    arm_ping(self); // schedule the next ping
  }

  void on_pong_timeout(Self self) {
    pong_timer_id_ = -1;
    if (awaiting_pong_ && !finished_ && !closing_) {
      report_error(std::make_error_code(std::errc::timed_out), self);
      finish_close(std::to_underlying(WsCloseCode::GoingAway), self);
    }
  }

  void on_pong(const Self& /*self*/) {
    awaiting_pong_ = false;
    if (pong_timer_id_ != -1) {
      reactor_->unregister_timer(pong_timer_id_);
      pong_timer_id_ = -1;
    }
  }

  // ── Teardown ────────────────────────────────────────────────────────────────

  // Graceful: queue a Close frame, stop reading, finish once flushed.
  void begin_close(uint16_t code, std::string_view reason, const Self& self) {
    if (closing_ || finished_) return;
    closing_ = true;
    close_code_ = code;

    // Stop reading further frames.
    if (read_armed_) {
      reactor_->unregister_event(fd_, EventType::Read);
      read_armed_ = false;
    }

    // Build the Close payload: 2-byte big-endian code + optional reason.
    std::array<std::byte, 125> payload{};
    size_t plen = 2;
    payload[0] = static_cast<std::byte>((code >> 8) & 0xFF);
    payload[1] = static_cast<std::byte>(code & 0xFF);
    const size_t rlen = std::min<size_t>(reason.size(), 123);
    for (size_t i = 0; i < rlen; ++i)
      payload[2 + i] = static_cast<std::byte>(reason[i]);
    plen += rlen;

    send_frame(WsFrame::Opcode::Close,
               std::span<const std::byte>(payload.data(), plen), &self);

    // If nothing is left to flush, finish immediately.
    if (!finished_ && sbuf_.size() == ssent_ && !write_armed_)
      finish_close(code, self);
  }

  // Terminal: unregister everything, fire on_close, drop from the registry.
  void finish_close(uint16_t code, const Self& self) {
    if (finished_) return;
    finished_ = true;
    closing_ = true;
    open_ = false;

    if (read_armed_)  { reactor_->unregister_event(fd_, EventType::Read);  read_armed_ = false; }
    if (write_armed_) { reactor_->unregister_event(fd_, EventType::Write); write_armed_ = false; }
    if (ping_timer_id_ != -1) { reactor_->unregister_timer(ping_timer_id_); ping_timer_id_ = -1; }
    if (pong_timer_id_ != -1) { reactor_->unregister_timer(pong_timer_id_); pong_timer_id_ = -1; }

    server_->dispatch_close(self, code);
    server_->deregister(id_);
    // The fd is closed by ~WsConnection once the last shared_ptr drops.
  }

  void report_error(std::error_code ec, const Self& self) {
    server_->dispatch_error(self, ec);
  }

  // ── State ───────────────────────────────────────────────────────────────────

  uint64_t       id_;
  int            fd_;
  Reactor*       reactor_;
  WsServer<Ctx>* server_;
  SocketAddr     remote_;
  Ctx            ctx_{};

  bool open_         = false;
  bool closing_      = false;
  bool finished_     = false;
  bool read_armed_   = false;
  bool write_armed_  = false;
  bool awaiting_pong_ = false;
  uint16_t close_code_ = std::to_underlying(WsCloseCode::Normal);
  int ping_timer_id_ = -1;
  int pong_timer_id_ = -1;

  // Inbound: raw byte accumulation + in-progress fragmented message.
  std::vector<uint8_t> rbuf_;
  std::vector<uint8_t> assembly_;
  uint8_t frag_opcode_ = 0; // 0 = no fragmented message in progress

  // Outbound: pending bytes [ssent_, sbuf_.size()).
  std::vector<std::byte> sbuf_;
  size_t ssent_ = 0;

  // Room membership (for O(rooms-of-this-conn) cleanup on close).
  std::unordered_set<std::string> rooms_;
};

// ─── WsHandlers<Ctx> ─────────────────────────────────────────────────────────

/**
 * @brief Application lifecycle callbacks for a `WsServer`.
 *
 * All callbacks run synchronously on the server's reactor thread. Keep
 * `on_message` fast (apply input, enqueue work) — for a game server the heavy
 * lifting belongs in the fixed-timestep simulation, not the I/O callback.
 *
 * @tparam Ctx Per-connection context type.
 */
template <class Ctx = std::monostate>
struct WsHandlers {
  using Conn = std::shared_ptr<WsConnection<Ctx>>;

  /** @brief Called once after a successful upgrade handshake. */
  std::function<void(Conn)> on_open;
  /** @brief Called for each complete (reassembled) Text/Binary message. */
  std::function<void(Conn, WsMessage)> on_message;
  /** @brief Called once when the connection is fully closed (with the close code). */
  std::function<void(Conn, uint16_t)> on_close;
  /** @brief Optional: called on an I/O / protocol / heartbeat error, before on_close. */
  std::function<void(Conn, std::error_code)> on_error;
};

// ─── WsServer<Ctx> ───────────────────────────────────────────────────────────

/**
 * @brief High-level, non-blocking WebSocket server.
 *
 * Owns the connection registry and rooms, dispatches lifecycle callbacks, and
 * provides broadcast. See the file-level docs for the threading model and a
 * usage example.
 *
 * @tparam Ctx Per-connection application context type.
 */
template <class Ctx = std::monostate>
class WsServer {
public:
  using Conn = std::shared_ptr<WsConnection<Ctx>>;

  explicit WsServer(WsHandlers<Ctx> handlers, WsServerConfig cfg = {})
      : handlers_(std::move(handlers)), cfg_(cfg) {}

  WsServer(const WsServer&)            = delete;
  WsServer& operator=(const WsServer&) = delete;

  /**
   * @brief Hard-terminates every connection and closes the listen socket.
   *
   * Connections hold a raw back-pointer to the server, so on destruction they
   * must be torn down *immediately* (all reactor callbacks/timers unregistered)
   * rather than via a graceful close that would leave a pending Write callback
   * dereferencing a freed server.
   *
   * @warning Destroy the `WsServer` **on its reactor thread, while that reactor
   *          is still alive** (i.e. before the owning `Dispatcher`/`Reactor`).
   *          Teardown unregisters events/timers through `reactor_`; destroying
   *          the reactor first leaves a dangling pointer.
   */
  ~WsServer() {
    std::vector<Conn> snapshot;
    snapshot.reserve(conns_.size());
    for (auto& [id, c] : conns_) snapshot.push_back(c);
    for (auto& c : snapshot)
      c->finish_close(std::to_underlying(WsCloseCode::GoingAway), c);
    if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
  }

  /** @brief The active configuration. */
  [[nodiscard]] const WsServerConfig& config() const noexcept { return cfg_; }
  /** @brief Current number of live connections. */
  [[nodiscard]] size_t connection_count() const noexcept { return conns_.size(); }
  /** @brief Look up a connection by id (nullptr if gone). */
  [[nodiscard]] Conn find(uint64_t id) const {
    auto it = conns_.find(id);
    return it == conns_.end() ? nullptr : it->second;
  }

  // ── Connection entry points ─────────────────────────────────────────────────

  /**
   * @brief Drive one already-accepted socket whose HTTP upgrade request was
   *        already parsed. Performs the handshake and starts the I/O loop.
   *
   * Must be called on the reactor thread that owns @p fd. Takes ownership of
   * @p fd (closes it on failure or when the connection ends).
   *
   * @param fd      Connected, soon-to-be-non-blocking socket.
   * @param req     The HTTP request carrying the WebSocket upgrade headers.
   * @param remote  Optional remote address (default-constructed if omitted).
   */
  [[nodiscard]] Result<void> serve_connection(int fd, const Request& req,
                                              SocketAddr remote = {}) {
    Reactor* r = Reactor::current();
    if (r == nullptr) {
      ::close(fd);
      return unexpected(std::make_error_code(std::errc::operation_not_permitted));
    }

    std::string_view key = req.header("Sec-WebSocket-Key");
    std::string_view ver = req.header("Sec-WebSocket-Version");
    if (key.empty() || ver != "13") {
      send_http_error(fd, "400 Bad Request");
      ::close(fd);
      return unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    if (conns_.size() >= cfg_.max_connections) {
      send_http_error(fd, "503 Service Unavailable");
      ::close(fd);
      return unexpected(std::make_error_code(std::errc::too_many_files_open));
    }

    set_nonblocking(fd);
    if (cfg_.tcp_nodelay) {
      int one = 1;
      ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }

    auto conn = std::make_shared<WsConnection<Ctx>>(
        typename WsConnection<Ctx>::PrivateTag{}, next_id_++, fd, r, this,
        remote);
    conns_.emplace(conn->id(), conn);

    std::string accept = WebSocketHandler::compute_accept_key(key);
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";

    conn->queue_handshake(response, conn);
    conn->start(conn);
    dispatch_open(conn);
    return {};
  }

  /**
   * @brief Adapter for the `Http1Handler` upgrade callback.
   *
   * @code
   * auto handler = std::make_unique<Http1Handler>(router, ws_server.upgrade_handler());
   * @endcode
   */
  [[nodiscard]] Http1Handler::UpgradeCallback upgrade_handler() {
    return [this](UpgradeRequest ureq) -> Task<void> {
      (void)serve_connection(ureq.fd, ureq.original_request);
      co_return;
    };
  }

  /**
   * @brief Bind, listen, accept, upgrade, and serve — a turnkey single-reactor
   *        WebSocket server.
   *
   * Runs on the current reactor until `stop()` is called. Each accepted socket
   * is read until its HTTP upgrade request is complete, then handed to
   * `serve_connection()`. For multi-core scaling, run one `WsServer::listen()`
   * per reactor thread on the same port (the listen socket uses SO_REUSEPORT).
   *
   * @param port IPv4 TCP port to listen on.
   */
  [[nodiscard]] Task<Result<void>> listen(uint16_t port) {
    Reactor* r = Reactor::current();
    if (r == nullptr)
      co_return unexpected(std::make_error_code(std::errc::operation_not_permitted));

    int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0)
      co_return unexpected(std::error_code(errno, std::system_category()));

    int one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
      std::error_code ec(errno, std::system_category());
      ::close(lfd);
      co_return unexpected(ec);
    }
    if (::listen(lfd, 1024) < 0) {
      std::error_code ec(errno, std::system_category());
      ::close(lfd);
      co_return unexpected(ec);
    }
    set_nonblocking(lfd);
    listen_fd_ = lfd;
    running_ = true;

    while (running_) {
      int cfd = co_await AsyncAccept{lfd};
      if (cfd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
        break;
      }
      set_nonblocking(cfd);
      begin_http_upgrade(cfd);
    }

    if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
    co_return Result<void>{};
  }

  /** @brief Stop the `listen()` accept loop (does not close existing connections). */
  void stop() { running_ = false; }

  // ── Broadcast / rooms ───────────────────────────────────────────────────────

  /** @brief Send a text message to every open connection. */
  void broadcast_text(std::string_view msg) {
    for (auto& [id, c] : conns_)
      if (c->is_open()) c->send_text(msg);
  }
  /** @brief Send a binary message to every open connection. */
  void broadcast_binary(std::span<const std::byte> data) {
    for (auto& [id, c] : conns_)
      if (c->is_open()) c->send_binary(data);
  }

  /** @brief Add a connection to a named room (creates the room if needed). */
  void join_room(uint64_t conn_id, std::string_view room) {
    auto it = conns_.find(conn_id);
    if (it == conns_.end()) return;
    std::string key{room};
    rooms_[key].insert(conn_id);
    it->second->rooms_.insert(std::move(key));
  }
  /** @brief Remove a connection from a named room. */
  void leave_room(uint64_t conn_id, std::string_view room) {
    std::string key{room};
    auto rit = rooms_.find(key);
    if (rit != rooms_.end()) {
      rit->second.erase(conn_id);
      if (rit->second.empty()) rooms_.erase(rit);
    }
    auto it = conns_.find(conn_id);
    if (it != conns_.end()) it->second->rooms_.erase(key);
  }
  /** @brief Send a text message to every open connection in a room. */
  void broadcast_room_text(std::string_view room, std::string_view msg) {
    auto rit = rooms_.find(std::string{room});
    if (rit == rooms_.end()) return;
    for (uint64_t id : rit->second) {
      auto it = conns_.find(id);
      if (it != conns_.end() && it->second->is_open()) it->second->send_text(msg);
    }
  }
  /** @brief Send a binary message to every open connection in a room. */
  void broadcast_room_binary(std::string_view room,
                             std::span<const std::byte> data) {
    auto rit = rooms_.find(std::string{room});
    if (rit == rooms_.end()) return;
    for (uint64_t id : rit->second) {
      auto it = conns_.find(id);
      if (it != conns_.end() && it->second->is_open()) it->second->send_binary(data);
    }
  }
  /** @brief Number of connections currently in a room. */
  [[nodiscard]] size_t room_size(std::string_view room) const {
    auto rit = rooms_.find(std::string{room});
    return rit == rooms_.end() ? 0 : rit->second.size();
  }

  /** @brief Begin a graceful close on every connection. */
  void close_all(WsCloseCode code = WsCloseCode::GoingAway) {
    // Copy the handles first: closing mutates conns_ via deregister().
    std::vector<Conn> snapshot;
    snapshot.reserve(conns_.size());
    for (auto& [id, c] : conns_) snapshot.push_back(c);
    for (auto& c : snapshot) c->close(code);
  }

private:
  friend class WsConnection<Ctx>;

  void dispatch_open(const Conn& c) {
    if (handlers_.on_open) handlers_.on_open(c);
  }
  void dispatch_message(const Conn& c, const WsMessage& m) {
    if (handlers_.on_message) handlers_.on_message(c, m);
  }
  void dispatch_close(const Conn& c, uint16_t code) {
    if (handlers_.on_close) handlers_.on_close(c, code);
  }
  void dispatch_error(const Conn& c, std::error_code ec) {
    if (handlers_.on_error) handlers_.on_error(c, ec);
  }

  // Remove a connection from the registry and any rooms it joined.
  void deregister(uint64_t id) {
    auto it = conns_.find(id);
    if (it == conns_.end()) return;
    for (const auto& room : it->second->rooms_) {
      auto rit = rooms_.find(room);
      if (rit != rooms_.end()) {
        rit->second.erase(id);
        if (rit->second.empty()) rooms_.erase(rit);
      }
    }
    conns_.erase(it);
  }

  // Per-accept HTTP upgrade reader: accumulate until the request parses, then
  // hand off to serve_connection(). Uses a small heap state object kept alive
  // by the reactor Read callback.
  void begin_http_upgrade(int cfd) {
    Reactor* r = Reactor::current();
    auto state = std::make_shared<UpgradeState>();
    WsServer* self = this;

    // Drop a stuck/slow pre-upgrade connection (slowloris): the post-upgrade
    // heartbeat doesn't cover this phase. `done` guards against the timer racing
    // a successful upgrade or an earlier close on the same reactor thread.
    if (cfg_.handshake_timeout_ms > 0) {
      auto tr = r->register_timer(cfg_.handshake_timeout_ms, [r, cfd, state](int) {
        if (state->done) return;
        state->done = true;
        r->unregister_event(cfd, EventType::Read);
        ::close(cfd);
      });
      if (tr) state->timer_id = *tr;
    }

    r->register_event(cfd, EventType::Read, [self, r, cfd, state](int fd) {
      if (state->done) return;
      auto fail = [&] {
        state->done = true;
        if (state->timer_id != -1) r->unregister_timer(state->timer_id);
        r->unregister_event(fd, EventType::Read);
        ::close(fd);
      };
      char tmp[4096];
      ssize_t n = ::read(fd, tmp, sizeof(tmp));
      if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        fail();
        return;
      }
      state->buf.append(tmp, static_cast<size_t>(n));
      if (state->buf.size() > 64 * 1024) { // upgrade request DoS guard
        fail();
        return;
      }
      HttpParser parser;
      Request req;
      auto parsed = parser.parse(state->buf, req);
      if (!parsed) {
        fail();
        return;
      }
      if (!parser.is_complete()) return; // need more bytes
      // Full request available — stop this temporary reader and upgrade.
      // Copy what we need into stack locals first: unregister_event destroys
      // this callback (and its captures). In particular `req`'s header values
      // are string_views into `state->buf`, so `state` must be kept alive on
      // the stack across the serve_connection() call that reads those headers.
      auto      keep_state = state; // NOLINT(performance-unnecessary-copy-initialization)
      WsServer* s = self;
      int       cf = cfd;
      Reactor*  rr = r;
      state->done = true;
      if (state->timer_id != -1) rr->unregister_timer(state->timer_id);
      rr->unregister_event(cf, EventType::Read);
      (void)s->serve_connection(cf, req);
    });
  }

  static void set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }

  static void send_http_error(int fd, std::string_view status_line) {
    std::string resp = "HTTP/1.1 ";
    resp += status_line;
    resp += "\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
    // Best-effort, blocking, tiny write on a connection we are about to close.
    [[maybe_unused]] ssize_t w = ::write(fd, resp.data(), resp.size());
  }

  struct UpgradeState {
    std::string buf;
    int  timer_id = -1;   ///< handshake-timeout timer (cancel on completion)
    bool done     = false; ///< set once the connection is closed or upgraded
  };

  WsHandlers<Ctx> handlers_;
  WsServerConfig  cfg_;
  uint64_t        next_id_ = 1;
  int             listen_fd_ = -1;
  bool            running_ = false;

  std::unordered_map<uint64_t, Conn> conns_;
  std::unordered_map<std::string, std::unordered_set<uint64_t>> rooms_;
};

} // namespace qbuem

/** @} */ // end of qbuem_ws_server
