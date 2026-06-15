/**
 * @file tests/server_e2e_test.cpp
 * @brief End-to-end coverage for the three remaining server surfaces:
 *
 *   A) WebSocket frame codec + RFC 6455 handshake (byte-level, no sockets).
 *      Exercises WebSocketHandler::compute_accept_key / encode_frame /
 *      decode_frame across every payload-length encoding, masked + unmasked,
 *      every control opcode, and the truncated-buffer contract.
 *
 *   B) Http1Handler response path over a socketpair(2).  A real GET request is
 *      driven through on_connect() + on_frame() on a single-thread Dispatcher;
 *      the bytes the handler writes to the fd are read back and asserted to
 *      contain the 200 status line and the routed body.
 *
 *   C) fetch() client end-to-end over the loopback interface.  A free-function
 *      server coroutine accepts one TCP connection, reads the request, and
 *      writes a fixed HTTP/1.1 response; the fetch() client coroutine parses it
 *      and the status + body are asserted.
 *
 * Concurrency model (identical to tests/net_loopback_test.cpp): a single-thread
 * Dispatcher (thread_count == 1) so every awaiter registers on the same
 * thread-local Reactor, avoiding cross-reactor resume.  All async work is driven
 * by FREE-FUNCTION coroutines (never immediately-invoked coroutine-lambdas) and
 * bounded by a wall-clock deadline via run_until().
 *
 * Test-side namespace adapter:
 *   server/http1_handler.hpp refers to qbuem::http::{Request,Response,Router},
 *   but the http value types live directly in namespace qbuem.  The library
 *   never includes http1_handler.hpp through a header that supplies the alias,
 *   so we provide the alias here (test-side only — no source file is modified)
 *   BEFORE including the handler header.
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/http/request.hpp>
#include <qbuem/http/response.hpp>
#include <qbuem/http/router.hpp>
#include <qbuem/net/socket_addr.hpp>
#include <qbuem/net/tcp_listener.hpp>
#include <qbuem/net/tcp_stream.hpp>
#include <qbuem/server/websocket_handler.hpp>

// Provide the qbuem::http alias the handler header expects (the http value
// types are declared directly in namespace qbuem).  Must precede the include.
namespace qbuem {
namespace http {
using qbuem::Request;
using qbuem::Response;
using qbuem::Router;
} // namespace http
} // namespace qbuem

#include <qbuem/http/fetch.hpp>
#include <qbuem/server/http1_handler.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;

namespace {

// Run the dispatcher's reactor thread until `done` is set or `timeout` elapses.
// Always stops the dispatcher and joins the thread before returning.  Returns
// true if `done` was observed set before the deadline (i.e. not a timeout).
bool run_until(Dispatcher &disp, const std::atomic<bool> &done,
               std::chrono::milliseconds timeout) {
  std::jthread t([&] { disp.run(); });
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!done.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(2ms);
  }
  const bool ok = done.load(std::memory_order_acquire);
  disp.stop();
  t.join();
  return ok;
}

// ─── A) WebSocket frame codec (byte-level) ──────────────────────────────────

// RFC 6455 §1.3 worked example: the canonical handshake test vector.
TEST(WebSocketCodec, HandshakeAcceptKeyVector) {
  const std::string accept =
      WebSocketHandler::compute_accept_key("dGhlIHNhbXBsZSBub25jZQ==");
  EXPECT_EQ(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

// Build a payload of an exact size filled with a deterministic byte pattern.
std::vector<uint8_t> make_payload(size_t n) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; ++i)
    v[i] = static_cast<uint8_t>((i * 31u + 7u) & 0xFFu);
  return v;
}

// Encode then decode one frame and assert the opcode and payload survive.
// `mask` controls client→server masking; decode_frame must unmask transparently
// so the recovered payload equals the original plaintext either way.
void roundtrip_one(WsFrame::Opcode opcode, size_t payload_size, bool mask) {
  WsFrame in;
  in.opcode = opcode;
  in.fin = true;
  in.masked = mask; // informational on the input struct; encode() honours `mask`
  in.payload = make_payload(payload_size);

  std::vector<uint8_t> bytes = WebSocketHandler::encode_frame(in, mask);
  ASSERT_FALSE(bytes.empty());

  size_t consumed = 0;
  auto decoded = WebSocketHandler::decode_frame(
      std::span<const uint8_t>(bytes.data(), bytes.size()), consumed);

  ASSERT_TRUE(decoded.has_value())
      << "decode failed for opcode="
      << static_cast<int>(std::to_underlying(opcode))
      << " size=" << payload_size << " mask=" << mask;
  EXPECT_EQ(consumed, bytes.size());
  EXPECT_EQ(decoded->opcode, opcode);
  EXPECT_EQ(decoded->masked, mask);
  ASSERT_EQ(decoded->payload.size(), payload_size);
  EXPECT_EQ(decoded->payload, in.payload)
      << "payload mismatch after roundtrip (mask=" << mask << ")";
}

// Data frames (Text/Binary), masked + unmasked, across the three length
// encodings: 1-byte (<126), 2-byte (126..65535), 8-byte (>65535).
TEST(WebSocketCodec, EncodeDecodeRoundtripAllLengthEncodings) {
  const size_t sizes[] = {
      5,      // 1-byte length field (< 126)
      200,    // 2-byte length field (126 .. 65535) — first boundary
      70000,  // 8-byte length field (> 65535)
  };
  for (bool mask : {false, true}) {
    for (size_t sz : sizes) {
      roundtrip_one(WsFrame::Opcode::Text, sz, mask);
      roundtrip_one(WsFrame::Opcode::Binary, sz, mask);
    }
  }
}

// Exact boundary values for the length-field transitions.
TEST(WebSocketCodec, EncodeDecodeRoundtripLengthBoundaries) {
  for (bool mask : {false, true}) {
    roundtrip_one(WsFrame::Opcode::Binary, 125, mask);   // last 1-byte form
    roundtrip_one(WsFrame::Opcode::Binary, 126, mask);   // first 2-byte form
    roundtrip_one(WsFrame::Opcode::Binary, 65535, mask); // last 2-byte form
    roundtrip_one(WsFrame::Opcode::Binary, 65536, mask); // first 8-byte form
  }
}

// Control opcodes (Ping/Pong/Close) must roundtrip with their (small) payloads.
TEST(WebSocketCodec, ControlOpcodesRoundtrip) {
  for (bool mask : {false, true}) {
    roundtrip_one(WsFrame::Opcode::Ping, 0, mask);
    roundtrip_one(WsFrame::Opcode::Pong, 4, mask);
    roundtrip_one(WsFrame::Opcode::Close, 2, mask); // 2-byte status code payload
  }
}

// decode_frame on an incomplete buffer must report "incomplete" (consumed==0)
// and never crash.  Per the header contract it returns an error Result with
// errc::resource_unavailable_try_again when fewer bytes than the frame needs
// are available.
TEST(WebSocketCodec, DecodeTruncatedBufferReportsIncomplete) {
  // A real masked frame with a 200-byte payload (2-byte length field).
  WsFrame in;
  in.opcode = WsFrame::Opcode::Binary;
  in.fin = true;
  in.payload = make_payload(200);
  std::vector<uint8_t> full = WebSocketHandler::encode_frame(in, /*mask=*/true);
  ASSERT_GT(full.size(), 8u);

  // Empty buffer: not even the 2-byte header is present.
  {
    size_t consumed = 123; // sentinel; must be reset to 0
    auto r = WebSocketHandler::decode_frame(
        std::span<const uint8_t>(full.data(), 0), consumed);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(consumed, 0u);
  }

  // 1 byte: less than the minimum header.
  {
    size_t consumed = 7;
    auto r = WebSocketHandler::decode_frame(
        std::span<const uint8_t>(full.data(), 1), consumed);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(consumed, 0u);
  }

  // Header + extended-length present but payload truncated to half.
  {
    size_t consumed = 99;
    const size_t half = full.size() / 2;
    auto r = WebSocketHandler::decode_frame(
        std::span<const uint8_t>(full.data(), half), consumed);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(consumed, 0u);
  }

  // Sanity: the full buffer DOES decode (proves the truncation is the only
  // reason the partial buffers failed).
  {
    size_t consumed = 0;
    auto r = WebSocketHandler::decode_frame(
        std::span<const uint8_t>(full.data(), full.size()), consumed);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(consumed, full.size());
    EXPECT_EQ(r->payload, in.payload);
  }
}

// ─── B) Http1Handler response path over a socketpair ────────────────────────

// Drives a single request through the handler.  The handler writes the response
// to sp[0]; the test reads it from sp[1] after `done` is set.
struct H1Result {
  std::atomic<bool> done{false};
};

// Free-function coroutine: connect the handler to one end of the socketpair and
// feed it a single GET /hi request, letting it write the response to that fd.
Task<void> drive_http1(std::shared_ptr<http::Router> router, int handler_fd,
                       H1Result &out) {
  Http1Handler handler(router);

  auto addr = SocketAddr::from_ipv4("127.0.0.1", 0);
  co_await handler.on_connect(handler_fd, addr ? *addr : SocketAddr{});

  Request req;
  req.set_method(Method::Get);
  req.set_path("/hi");
  // on_frame() inspects Connection/Upgrade headers; with none set it routes and
  // writes "Connection: keep-alive" — harmless for this one-shot read.
  co_await handler.on_frame(std::move(req));

  out.done.store(true, std::memory_order_release);
  co_return;
}

TEST(Http1Handler, RoutedResponseWrittenToSocket) {
  int sp[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sp), 0)
      << "socketpair failed: " << std::strerror(errno);

  auto router = std::make_shared<http::Router>();
  router->add_route(Method::Get, "/hi",
                    [](const Request &, Response &res) {
                      res.status(200).body("pong");
                    });

  Dispatcher disp(1);
  H1Result out;
  disp.spawn(drive_http1(router, sp[0], out));

  const bool completed = run_until(disp, out.done, 3000ms);
  ASSERT_TRUE(completed) << "Http1Handler drive timed out";

  // Read whatever the handler wrote.  The peer end (sp[0]) is still open
  // (the TcpStream-less handler does not close it), so read() may block; use a
  // bounded read after `done` — the bytes are already buffered by the kernel.
  std::array<char, 1024> buf{};
  ssize_t n = ::read(sp[1], buf.data(), buf.size());
  ASSERT_GT(n, 0) << "no response bytes read from handler";

  std::string response(buf.data(), static_cast<size_t>(n));
  EXPECT_NE(response.find("200"), std::string::npos)
      << "response missing 200 status: " << response;
  EXPECT_NE(response.find("pong"), std::string::npos)
      << "response missing routed body: " << response;

  ::close(sp[0]);
  ::close(sp[1]);
}

// ─── C) fetch() client end-to-end over loopback ─────────────────────────────

constexpr uint16_t kFetchPort = 39517; // fixed high port unlikely to clash

// Slot for the client-observed result.
struct FetchOutcome {
  std::atomic<bool> done{false};
  std::atomic<bool> error{false};
  std::atomic<int> status{0};
  std::array<char, 256> body{};
  std::atomic<size_t> body_len{0};
};

// Free-function server coroutine: accept one connection, read the request bytes
// once, then write a valid fixed HTTP/1.1 response (Connection: close so the
// fetch client's read-until-close termination path completes deterministically).
Task<void> fetch_raw_server(TcpListener &listener) {
  auto client = co_await listener.accept();
  if (!client) co_return;

  std::array<std::byte, 2048> reqbuf{};
  // Drain the request line + headers (single read is sufficient for a small GET).
  auto r = co_await client->read(reqbuf);
  (void)r;

  static constexpr std::string_view kResp =
      "HTTP/1.1 200 OK\r\n"
      "Content-Length: 4\r\n"
      "Connection: close\r\n"
      "\r\n"
      "pong";
  std::string_view remaining = kResp;
  while (!remaining.empty()) {
    auto w = co_await client->write(std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(remaining.data()),
        remaining.size()));
    if (!w || *w == 0) break;
    remaining.remove_prefix(*w);
  }
  co_return;
}

// Free-function client coroutine: fetch the loopback URL and record the result.
Task<void> fetch_client(FetchOutcome &out) {
  auto r = co_await fetch("http://127.0.0.1:39517/")
               .timeout(std::chrono::seconds{2})
               .send(std::stop_token{});
  if (!r) {
    out.error.store(true, std::memory_order_relaxed);
    out.done.store(true, std::memory_order_release);
    co_return;
  }
  out.status.store(r->status(), std::memory_order_relaxed);
  std::string_view body = r->body();
  const size_t n = std::min(body.size(), out.body.size());
  std::memcpy(out.body.data(), body.data(), n);
  out.body_len.store(n, std::memory_order_relaxed);
  out.done.store(true, std::memory_order_release);
  co_return;
}

TEST(FetchE2E, GetOverLoopbackReturnsBody) {
  auto addr = SocketAddr::from_ipv4("127.0.0.1", kFetchPort);
  ASSERT_TRUE(addr.has_value());

  auto listener = TcpListener::bind(*addr);
  ASSERT_TRUE(listener.has_value()) << "TcpListener::bind failed";

  Dispatcher disp(1);
  FetchOutcome out;

  // Server coroutine registers its accept first.
  disp.spawn(fetch_raw_server(*listener));

  // Defer the client spawn so the accept is in place on the reactor thread,
  // mirroring net_loopback_test's 40ms stagger.
  std::jthread spawn_client([&] {
    std::this_thread::sleep_for(40ms);
    disp.spawn(fetch_client(out));
  });

  const bool completed = run_until(disp, out.done, 3000ms);
  spawn_client.join();

  ASSERT_TRUE(completed) << "fetch round trip timed out";
  ASSERT_FALSE(out.error.load()) << "fetch().send() returned an error";
  EXPECT_EQ(out.status.load(), 200);

  std::string body(out.body.data(), out.body_len.load());
  EXPECT_EQ(body, "pong");
}

} // namespace
