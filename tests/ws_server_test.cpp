/**
 * @file tests/ws_server_test.cpp
 * @brief Coverage for the high-level WsServer (include/qbuem/server/ws_server.hpp).
 *
 *   A) Pure units (no sockets): ws_is_valid_utf8() edge cases and
 *      WebSocketHandler::encode_header() length-encoding correctness.
 *
 *   B) WsServer over a socketpair(2), driven on a single-thread Dispatcher
 *      (every awaiter/callback registers on the same thread-local Reactor —
 *      no cross-reactor resume). A blocking client thread performs the
 *      handshake and exercises: echo, fragmentation reassembly, Ping→Pong,
 *      the Close handshake, and a strict-protocol rejection (unmasked frame).
 *
 *   C) WsServer::listen() end-to-end over loopback: accept → HTTP upgrade
 *      parse → handshake → echo (smoke test; skipped if the port is busy).
 *
 * The server lives on the reactor thread; the client uses blocking I/O on a
 * separate OS thread with SO_RCVTIMEO so a missing reply fails fast instead of
 * hanging. Connections are always torn down on the reactor thread (client
 * sends Close / closes the socket) before the server is destroyed.
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/http/parser.hpp>
#include <qbuem/http/request.hpp>
#include <qbuem/server/ws_server.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;

namespace {

// ─── Test harness ────────────────────────────────────────────────────────────

bool run_until(Dispatcher& disp, const std::atomic<bool>& done,
               std::chrono::milliseconds timeout) {
  std::jthread t([&] { disp.run(); });
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  bool ok = false;
  while (std::chrono::steady_clock::now() < deadline) {
    if (done.load(std::memory_order_acquire)) { ok = true; break; }
    std::this_thread::sleep_for(2ms);
  }
  disp.stop();
  return ok;
}

// A blocking WebSocket client over a raw fd. Server→client frames are unmasked;
// client→server frames are masked (RFC 6455 §5.1).
struct BlockingWsClient {
  int fd;
  std::vector<uint8_t> buf;

  explicit BlockingWsClient(int f) : fd(f) {
    timeval tv{};
    tv.tv_sec = 2;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }

  // Read and discard the HTTP/1.1 101 response (through the blank line).
  bool read_handshake(std::string& out) {
    out.clear();
    char c;
    while (out.find("\r\n\r\n") == std::string::npos) {
      ssize_t n = ::read(fd, &c, 1);
      if (n <= 0) return false;
      out.push_back(c);
      if (out.size() > 4096) return false;
    }
    return true;
  }

  void send(WsFrame::Opcode op, std::string_view payload, bool fin = true) {
    WsFrame f;
    f.opcode = op;
    f.fin = fin;
    f.payload.assign(payload.begin(), payload.end());
    auto bytes = WebSocketHandler::encode_frame(f, /*mask=*/true);
    size_t off = 0;
    while (off < bytes.size()) {
      ssize_t n = ::write(fd, bytes.data() + off, bytes.size() - off);
      if (n <= 0) return;
      off += static_cast<size_t>(n);
    }
  }

  // Send a raw (possibly malformed) frame verbatim.
  void send_raw(std::span<const uint8_t> bytes) {
    size_t off = 0;
    while (off < bytes.size()) {
      ssize_t n = ::write(fd, bytes.data() + off, bytes.size() - off);
      if (n <= 0) return;
      off += static_cast<size_t>(n);
    }
  }

  // Read the next complete frame from the server.
  bool recv(WsFrame& out) {
    for (;;) {
      size_t consumed = 0;
      auto r = WebSocketHandler::decode_frame(
          std::span<const uint8_t>(buf.data(), buf.size()), consumed);
      if (r) {
        out = std::move(*r);
        buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(consumed));
        return true;
      }
      uint8_t tmp[2048];
      ssize_t n = ::read(fd, tmp, sizeof(tmp));
      if (n <= 0) return false;
      buf.insert(buf.end(), tmp, tmp + n);
    }
  }
};

std::string frame_text(const WsFrame& f) {
  return std::string(f.payload.begin(), f.payload.end());
}

// Build a minimal HTTP/1.1 WebSocket upgrade request and parse it into a Request.
Request make_upgrade_request() {
  static constexpr std::string_view raw =
      "GET /ws HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "\r\n";
  HttpParser parser;
  Request req;
  (void)parser.parse(raw, req);
  return req;
}

// Coroutine that upgrades a server-side fd on the reactor thread.
Task<void> serve_on_reactor(WsServer<>* server, int fd, Request req) {
  (void)server->serve_connection(fd, std::move(req));
  co_return;
}

} // namespace

// ─── A) Pure units ───────────────────────────────────────────────────────────

TEST(WsUtf8, AcceptsValidSequences) {
  auto ok = [](std::string_view s) {
    return ws_is_valid_utf8(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  };
  EXPECT_TRUE(ok(""));
  EXPECT_TRUE(ok("hello world"));
  EXPECT_TRUE(ok("\xC3\xA9"));             // é  (U+00E9, 2 bytes)
  EXPECT_TRUE(ok("\xE2\x82\xAC"));         // €  (U+20AC, 3 bytes)
  EXPECT_TRUE(ok("\xF0\x9F\x98\x80"));     // 😀 (U+1F600, 4 bytes)
  EXPECT_TRUE(ok("\xEC\x95\x88\xEB\x85\x95")); // "안녕" (Korean)
}

TEST(WsUtf8, RejectsMalformedSequences) {
  auto bad = [](std::initializer_list<uint8_t> bytes) {
    std::vector<uint8_t> v(bytes);
    return !ws_is_valid_utf8(v.data(), v.size());
  };
  EXPECT_TRUE(bad({0x80}));                   // lone continuation byte
  EXPECT_TRUE(bad({0xC0, 0x80}));             // overlong 2-byte
  EXPECT_TRUE(bad({0xE0, 0x80, 0x80}));       // overlong 3-byte
  EXPECT_TRUE(bad({0xF0, 0x80, 0x80, 0x80})); // overlong 4-byte
  EXPECT_TRUE(bad({0xED, 0xA0, 0x80}));       // UTF-16 surrogate U+D800
  EXPECT_TRUE(bad({0xF5, 0x80, 0x80, 0x80})); // > U+10FFFF
  EXPECT_TRUE(bad({0xC3}));                    // truncated 2-byte
  EXPECT_TRUE(bad({0xE2, 0x82}));             // truncated 3-byte
  EXPECT_TRUE(bad({0xC3, 0x28}));             // bad continuation
}

TEST(WsEncodeHeader, LengthEncodings) {
  std::array<uint8_t, WebSocketHandler::kMaxHeaderBytes> h{};

  // < 126 → 2-byte header, FIN + Binary.
  size_t n = WebSocketHandler::encode_header(h.data(), WsFrame::Opcode::Binary,
                                             5, /*fin=*/true, /*mask=*/false);
  EXPECT_EQ(n, 2u);
  EXPECT_EQ(h[0], 0x82);            // FIN | Binary
  EXPECT_EQ(h[1], 5);

  // 126..65535 → 4-byte header.
  n = WebSocketHandler::encode_header(h.data(), WsFrame::Opcode::Text, 200);
  EXPECT_EQ(n, 4u);
  EXPECT_EQ(h[0], 0x81);            // FIN | Text
  EXPECT_EQ(h[1], 126);
  EXPECT_EQ((h[2] << 8) | h[3], 200);

  // > 65535 → 10-byte header.
  n = WebSocketHandler::encode_header(h.data(), WsFrame::Opcode::Binary, 70000);
  EXPECT_EQ(n, 10u);
  EXPECT_EQ(h[1], 127);

  // Masked → +4 bytes for the key, MASK bit set.
  std::array<uint8_t, 4> key{1, 2, 3, 4};
  n = WebSocketHandler::encode_header(h.data(), WsFrame::Opcode::Text, 1,
                                      /*fin=*/true, /*mask=*/true, key);
  EXPECT_EQ(n, 6u);
  EXPECT_EQ(h[1] & 0x80, 0x80);
}

TEST(WsDecodeFrame, ExposesRsvBits) {
  // FIN + RSV1 + Text, unmasked, empty payload.
  std::array<uint8_t, 2> bytes{0xC1, 0x00};
  size_t consumed = 0;
  auto r = WebSocketHandler::decode_frame(bytes, consumed);
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(r->rsv1);
  EXPECT_FALSE(r->rsv2);
  EXPECT_FALSE(r->rsv3);
  EXPECT_EQ(consumed, 2u);
}

// ─── B) WsServer over socketpair ─────────────────────────────────────────────

TEST(WsServer, HandshakeEchoFragmentationControlClose) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  std::atomic<bool> opened{false};
  std::atomic<bool> closed{false};
  std::atomic<int>  msg_count{0};
  std::vector<std::string> messages; // reactor-thread only

  // Dispatcher first → the reactor outlives the WsServer destructor.
  Dispatcher disp(1);
  WsServer<> server({
      .on_open    = [&](auto) { opened.store(true); },
      .on_message = [&](auto conn, WsMessage m) {
        messages.emplace_back(m.text());
        msg_count.fetch_add(1);
        conn->send_text(m.text()); // echo
      },
      .on_close   = [&](auto, uint16_t) { closed.store(true); },
  });

  disp.spawn(serve_on_reactor(&server, sv[0], make_upgrade_request()));

  std::string client_log;
  std::jthread client([&] {
    BlockingWsClient c(sv[1]);
    std::string hs;
    if (!c.read_handshake(hs)) { client_log = "no-handshake"; return; }
    if (hs.find("101 Switching Protocols") == std::string::npos ||
        hs.find("Sec-WebSocket-Accept:") == std::string::npos) {
      client_log = "bad-handshake";
      return;
    }

    WsFrame f;

    // 1) Single-frame echo.
    c.send(WsFrame::Opcode::Text, "hello");
    if (!c.recv(f) || frame_text(f) != "hello") { client_log = "echo-fail"; return; }

    // 2) Fragmented message: "foo" (fin=0) + "bar" (continuation, fin=1).
    c.send(WsFrame::Opcode::Text, "foo", /*fin=*/false);
    c.send(WsFrame::Opcode::Continuation, "bar", /*fin=*/true);
    if (!c.recv(f) || frame_text(f) != "foobar") { client_log = "frag-fail"; return; }

    // 3) Ping → Pong (payload echoed).
    c.send(WsFrame::Opcode::Ping, "pq");
    if (!c.recv(f) || f.opcode != WsFrame::Opcode::Pong ||
        frame_text(f) != "pq") { client_log = "ping-fail"; return; }

    // 4) Close handshake.
    c.send(WsFrame::Opcode::Close, "");
    if (!c.recv(f) || f.opcode != WsFrame::Opcode::Close) {
      client_log = "close-fail";
      return;
    }
    client_log = "ok";
  });

  bool ok = run_until(disp, closed, 3s);
  client.join();

  EXPECT_TRUE(ok) << "server never reported close";
  EXPECT_EQ(client_log, "ok");
  EXPECT_TRUE(opened.load());
  EXPECT_TRUE(closed.load());
  ASSERT_EQ(msg_count.load(), 2);
  EXPECT_EQ(messages[0], "hello");
  EXPECT_EQ(messages[1], "foobar");
}

TEST(WsServer, RejectsUnmaskedClientFrame) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  std::atomic<bool> closed{false};
  Dispatcher disp(1);
  WsServer<> server({
      .on_close = [&](auto, uint16_t code) {
        EXPECT_EQ(code, std::to_underlying(WsCloseCode::ProtocolError));
        closed.store(true);
      },
  });

  disp.spawn(serve_on_reactor(&server, sv[0], make_upgrade_request()));

  std::string client_log;
  std::jthread client([&] {
    BlockingWsClient c(sv[1]);
    std::string hs;
    if (!c.read_handshake(hs)) { client_log = "no-handshake"; return; }
    // Unmasked Text frame "hi" — server MUST close with 1002.
    std::array<uint8_t, 4> raw{0x81, 0x02, 'h', 'i'};
    c.send_raw(raw);
    WsFrame f;
    client_log = (c.recv(f) && f.opcode == WsFrame::Opcode::Close) ? "ok"
                                                                   : "no-close";
  });

  bool ok = run_until(disp, closed, 3s);
  client.join();
  EXPECT_TRUE(ok);
  EXPECT_EQ(client_log, "ok");
}

TEST(WsServer, BroadcastAndRooms) {
  // Two server-side connections; broadcast to a room reaches only its members.
  int a[2], b[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, a), 0);
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, b), 0);

  // Dispatcher declared BEFORE the server so the reactor outlives WsServer's
  // destructor (teardown unregisters events through the reactor).
  Dispatcher disp(1);

  std::atomic<int> opened{0};
  std::atomic<bool> all_closed{false};
  int closed_count = 0; // reactor-thread only
  WsServer<>* server_ptr = nullptr;

  WsServer<> server({
      .on_open = [&](auto conn) {
        // First connection joins room "red"; second does not.
        if (opened.load() == 0) server_ptr->join_room(conn->id(), "red");
        opened.fetch_add(1);
      },
      .on_close = [&](auto, uint16_t) {
        if (++closed_count == 2) all_closed.store(true);
      },
  });
  server_ptr = &server;

  disp.spawn(serve_on_reactor(&server, a[0], make_upgrade_request()));
  disp.spawn(serve_on_reactor(&server, b[0], make_upgrade_request()));

  // Wait until both connections are open, then broadcast from the reactor.
  disp.spawn([](WsServer<>* s, std::atomic<int>* op) -> Task<void> {
    for (int i = 0; i < 200 && op->load() < 2; ++i) co_await sleep(5);
    s->broadcast_room_text("red", "to-red");   // only conn A
    s->broadcast_text("to-all");               // both
    co_return;
  }(&server, &opened));

  std::string a_log, b_log;

  std::jthread ca([&] {
    BlockingWsClient c(a[1]);
    std::string hs; c.read_handshake(hs);
    WsFrame f1, f2;
    bool g1 = c.recv(f1), g2 = c.recv(f2);
    a_log = (g1 && g2 && frame_text(f1) == "to-red" && frame_text(f2) == "to-all")
                ? "ok" : "bad";
    ::close(a[1]); // drive server-side teardown on the reactor thread
  });
  std::jthread cb([&] {
    BlockingWsClient c(b[1]);
    std::string hs; c.read_handshake(hs);
    WsFrame f1;
    bool g1 = c.recv(f1);              // should be "to-all" (NOT "to-red")
    b_log = (g1 && frame_text(f1) == "to-all") ? "ok" : "bad";
    ::close(b[1]);
  });

  bool ok = run_until(disp, all_closed, 3s);
  ca.join(); cb.join();

  EXPECT_TRUE(ok) << "both connections should be torn down on the reactor";
  EXPECT_EQ(a_log, "ok");
  EXPECT_EQ(b_log, "ok");
}

// ─── C) listen() end-to-end (loopback) ───────────────────────────────────────

TEST(WsServer, ListenAcceptUpgradeEcho) {
  constexpr uint16_t kPort = 47913;

  std::atomic<bool> echoed{false};
  Dispatcher disp(1);
  WsServer<> server({
      .on_message = [&](auto conn, WsMessage m) { conn->send_text(m.text()); },
  });

  // Pre-flight bind check: skip if the port is busy on this machine.
  {
    int probe = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(probe, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in pa{};
    pa.sin_family = AF_INET;
    pa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    pa.sin_port = htons(kPort);
    int br = ::bind(probe, reinterpret_cast<sockaddr*>(&pa), sizeof(pa));
    ::close(probe);
    if (br != 0) GTEST_SKIP() << "port " << kPort << " busy";
  }

  disp.spawn([](WsServer<>* s) -> Task<void> {
    (void)co_await s->listen(kPort);
    co_return;
  }(&server));

  std::string client_log;
  std::jthread client([&] {
    std::this_thread::sleep_for(60ms); // let listen() bind
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(kPort);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
      client_log = "connect-fail";
      ::close(fd);
      return;
    }
    // Send the HTTP upgrade request.
    static constexpr std::string_view req =
        "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    (void)::write(fd, req.data(), req.size());

    BlockingWsClient c(fd);
    std::string hs;
    if (!c.read_handshake(hs) ||
        hs.find("101 Switching Protocols") == std::string::npos) {
      client_log = "handshake-fail";
      ::close(fd);
      return;
    }
    c.send(WsFrame::Opcode::Text, "ping-pong");
    WsFrame f;
    if (c.recv(f) && frame_text(f) == "ping-pong") {
      echoed.store(true);
      client_log = "ok";
    } else {
      client_log = "echo-fail";
    }
    c.send(WsFrame::Opcode::Close, "");
    ::close(fd);
  });

  run_until(disp, echoed, 4s);
  server.stop();
  client.join();
  EXPECT_EQ(client_log, "ok");
  EXPECT_TRUE(echoed.load());
}
