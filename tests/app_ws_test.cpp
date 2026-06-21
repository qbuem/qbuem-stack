/**
 * @file tests/app_ws_test.cpp
 * @brief End-to-end test for App::ws() — WebSocket served on the same App/port
 *        as HTTP, with no hand-wired upgrade callback.
 *
 * A single-reactor App registers one HTTP route and one ws() route, listens on a
 * loopback port in a background thread, and a blocking client (1) GETs the HTTP
 * route and (2) performs a WS handshake + echo + close. Asserts the WS lifecycle
 * callbacks fire and that normal HTTP still works on the same port.
 */

#include <qbuem/qbuem_stack.hpp>
#include <qbuem/server/websocket_handler.hpp> // codec for the test client

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

using namespace qbuem;
using namespace std::chrono_literals;

namespace {

constexpr uint16_t kPort = 48231;

// Connect a blocking TCP socket to the loopback port (with a recv timeout).
int connect_loopback(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  timeval tv{2, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  sa.sin_port = htons(port);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

bool write_all_fd(int fd, std::string_view s) {
  size_t off = 0;
  while (off < s.size()) {
    ssize_t n = ::write(fd, s.data() + off, s.size() - off);
    if (n <= 0) return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

// Read until the HTTP header terminator (or a WS frame can be decoded).
std::string read_some(int fd, size_t max = 4096) {
  std::string out;
  char buf[1024];
  for (int i = 0; i < 8; ++i) {
    ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n <= 0) break;
    out.append(buf, static_cast<size_t>(n));
    if (out.size() >= max || out.find("\r\n\r\n") != std::string::npos) break;
  }
  return out;
}

} // namespace

TEST(AppWs, ServesWebSocketAndHttpOnSamePort) {
  // Single-reactor App so the whole test runs deterministically on one thread.
  App app{1};

  std::atomic<bool> opened{false};
  std::atomic<bool> closed{false};
  std::atomic<int>  messages{0};

  app.get("/hello", [](const Request&, Response& res) {
    res.status(200).body("hi-http");
  });

  app.ws("/ws", {
    .on_open    = [&](auto conn) { opened.store(true); conn->send_text("welcome"); },
    .on_message = [&](auto conn, WsMessage m) {
      messages.fetch_add(1);
      conn->send_text(std::string(m.text())); // echo
    },
    .on_close   = [&](auto, uint16_t) { closed.store(true); },
  });

  std::jthread server([&] { (void)app.listen(kPort); });
  std::this_thread::sleep_for(150ms); // let listen() bind

  // ── 1. Plain HTTP on the same port still works ────────────────────────────
  {
    int fd = connect_loopback(kPort);
    ASSERT_GE(fd, 0) << "HTTP connect failed";
    ASSERT_TRUE(write_all_fd(fd,
        "GET /hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));
    std::string resp = read_some(fd);
    EXPECT_NE(resp.find("200"), std::string::npos);
    EXPECT_NE(resp.find("hi-http"), std::string::npos);
    ::close(fd);
  }

  // ── 2. WebSocket upgrade + welcome + echo + close on the same port ────────
  std::string client_log = "init";
  {
    int fd = connect_loopback(kPort);
    ASSERT_GE(fd, 0) << "WS connect failed";
    ASSERT_TRUE(write_all_fd(fd,
        "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"));

    // Read the 101 handshake (up to the blank line).
    std::string hs;
    {
      char c;
      while (hs.find("\r\n\r\n") == std::string::npos && hs.size() < 1024) {
        ssize_t n = ::read(fd, &c, 1);
        if (n <= 0) break;
        hs.push_back(c);
      }
    }
    ASSERT_NE(hs.find("101 Switching Protocols"), std::string::npos) << hs;
    ASSERT_NE(hs.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="),
              std::string::npos);

    // Decode frames from the socket via the library codec.
    std::vector<uint8_t> rbuf;
    auto recv_frame = [&](WsFrame& out) -> bool {
      for (;;) {
        size_t consumed = 0;
        auto r = WebSocketHandler::decode_frame(
            std::span<const uint8_t>(rbuf.data(), rbuf.size()), consumed);
        if (r) {
          out = std::move(*r);
          rbuf.erase(rbuf.begin(), rbuf.begin() + static_cast<std::ptrdiff_t>(consumed));
          return true;
        }
        uint8_t tmp[1024];
        ssize_t n = ::read(fd, tmp, sizeof(tmp));
        if (n <= 0) return false;
        rbuf.insert(rbuf.end(), tmp, tmp + n);
      }
    };
    auto send_frame = [&](WsFrame::Opcode op, std::string_view payload) {
      WsFrame f;
      f.opcode = op; f.fin = true;
      f.payload.assign(payload.begin(), payload.end());
      auto bytes = WebSocketHandler::encode_frame(f, /*mask=*/true);
      write_all_fd(fd, std::string_view(
          reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    };

    WsFrame f;
    bool ok = true;
    // on_open pushed "welcome"
    ok = ok && recv_frame(f) && std::string(f.payload.begin(), f.payload.end()) == "welcome";
    // echo
    send_frame(WsFrame::Opcode::Text, "ping");
    ok = ok && recv_frame(f) && std::string(f.payload.begin(), f.payload.end()) == "ping";
    // close handshake
    send_frame(WsFrame::Opcode::Close, "");
    ok = ok && recv_frame(f) && f.opcode == WsFrame::Opcode::Close;
    client_log = ok ? "ok" : "bad";
    ::close(fd);
  }

  // Give the reactor a moment to run on_close after the client closed.
  std::this_thread::sleep_for(100ms);
  app.stop();
  server.join();

  EXPECT_EQ(client_log, "ok");
  EXPECT_TRUE(opened.load())   << "on_open did not fire";
  EXPECT_EQ(messages.load(), 1) << "on_message count";
  EXPECT_TRUE(closed.load())   << "on_close did not fire";
}
