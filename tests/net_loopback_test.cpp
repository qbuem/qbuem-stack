/**
 * @file tests/net_loopback_test.cpp
 * @brief Loopback round-trip tests for TcpListener/TcpStream, UnixSocket, and
 *        UdpSocket driven through a single-threaded Dispatcher.
 *
 * Each test:
 *   - binds a server-side socket,
 *   - spawns server + client coroutines on the same Dispatcher reactor thread,
 *   - sends a known byte payload one way and reads it back,
 *   - asserts the received bytes equal the sent bytes (EXPECT_EQ on real data).
 *
 * Every test is bounded by a wall-clock deadline (`run_until`) so the reactor
 * thread is always stopped and the process always terminates, even if the
 * library never completes the round trip.
 *
 * A single-thread Dispatcher (thread_count == 1) is used deliberately: the I/O
 * awaiters register events on `Reactor::current()`, which is thread-local. With
 * one reactor, the listener accept and the stream read/write all share the same
 * reactor, avoiding cross-reactor resume hazards.
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/net/socket_addr.hpp>
#include <qbuem/net/tcp_listener.hpp>
#include <qbuem/net/tcp_stream.hpp>
#include <qbuem/net/udp_socket.hpp>
#include <qbuem/net/unix_socket.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;

namespace {

// ─── Helpers ────────────────────────────────────────────────────────────────

// Convert a string_view to a span of const bytes for the write/send APIs.
std::span<const std::byte> as_bytes_view(std::string_view sv) {
  return std::as_bytes(std::span(sv.data(), sv.size()));
}

// Reconstruct a std::string from a received byte buffer prefix.
std::string to_string(const std::byte *data, size_t n) {
  return std::string(reinterpret_cast<const char *>(data), n);
}

// Run the dispatcher's reactor thread until `done` is set or `timeout` elapses.
// Always stops the dispatcher and joins the thread before returning. Returns
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

// Shared result slot for a single round trip. The server writes the bytes it
// observed into `payload`/`len`, sets `error` if anything failed, then sets
// `done` last (release) so the main thread reads a consistent snapshot.
struct RoundTrip {
  std::atomic<bool> done{false};
  std::atomic<bool> error{false};
  std::atomic<size_t> len{0};
  std::array<std::byte, 256> payload{};
  std::atomic<uint16_t> peer_port{0};
};

// ─── TCP loopback ─────────────────────────────────────────────────────────────

constexpr uint16_t kTcpPort = 53917; // fixed high port for loopback

// Server: accept one connection, read one chunk, copy it into `rt`.
Task<void> tcp_server(TcpListener &listener, RoundTrip &rt) {
  auto client = co_await listener.accept();
  if (!client) {
    rt.error.store(true, std::memory_order_relaxed);
    rt.done.store(true, std::memory_order_release);
    co_return;
  }

  std::array<std::byte, 256> buf{};
  auto n = co_await client->read(buf);
  if (!n || *n == 0) {
    rt.error.store(true, std::memory_order_relaxed);
    rt.done.store(true, std::memory_order_release);
    co_return;
  }

  const size_t got = *n;
  std::memcpy(rt.payload.data(), buf.data(), got);
  rt.len.store(got, std::memory_order_relaxed);
  rt.done.store(true, std::memory_order_release);
  co_return;
}

// Client: connect, write the payload once.
Task<void> tcp_client(SocketAddr addr, std::string_view payload,
                      std::atomic<bool> &write_failed) {
  auto stream = co_await TcpStream::connect(addr);
  if (!stream) {
    write_failed.store(true, std::memory_order_relaxed);
    co_return;
  }
  auto w = co_await stream->write(as_bytes_view(payload));
  if (!w || *w != payload.size())
    write_failed.store(true, std::memory_order_relaxed);
  // Keep the stream alive until the server has read; the dispatcher deadline
  // bounds total lifetime regardless.
  co_return;
}

TEST(NetLoopback, TcpSendAndReadBack) {
  auto addr = SocketAddr::from_ipv4("127.0.0.1", kTcpPort);
  ASSERT_TRUE(addr.has_value());

  auto listener = TcpListener::bind(*addr);
  ASSERT_TRUE(listener.has_value()) << "TcpListener::bind failed";

  constexpr std::string_view kMsg = "tcp-loopback-payload-0123456789";

  Dispatcher disp(1);
  RoundTrip rt;
  std::atomic<bool> client_write_failed{false};

  disp.spawn(tcp_server(*listener, rt));

  // The reactor thread must be running and the accept registered before the
  // client connects. Defer the client spawn so the accept is in place.
  std::jthread spawn_client([&] {
    std::this_thread::sleep_for(40ms);
    disp.spawn(tcp_client(*addr, kMsg, client_write_failed));
  });

  const bool completed = run_until(disp, rt.done, 3000ms);
  spawn_client.join();

  ASSERT_TRUE(completed) << "TCP round trip timed out";
  EXPECT_FALSE(rt.error.load()) << "server-side accept/read failed";
  EXPECT_FALSE(client_write_failed.load()) << "client connect/write failed";

  const size_t n = rt.len.load();
  EXPECT_EQ(n, kMsg.size());
  EXPECT_EQ(to_string(rt.payload.data(), n), std::string(kMsg));
}

// ─── TcpStream::write_all / read_exact (high-level helpers) ───────────────────

constexpr uint16_t kTcpPort2 = 53919;
constexpr size_t   kBigLen   = 64 * 1024; // large enough to span multiple reads

// Server: accept, read EXACTLY kBigLen bytes via read_exact, record success +
// a simple checksum so a partial/misordered read would be detected.
Task<void> tcp_server_exact(TcpListener &listener, RoundTrip &rt) {
  auto client = co_await listener.accept();
  if (!client) {
    rt.error.store(true); rt.done.store(true, std::memory_order_release); co_return;
  }
  std::vector<std::byte> buf(kBigLen);
  auto r = co_await client->read_exact(buf);
  if (!r) {
    rt.error.store(true); rt.done.store(true, std::memory_order_release); co_return;
  }
  uint32_t sum = 0;
  for (std::byte b : buf) sum += static_cast<uint8_t>(b);
  rt.len.store(sum);
  rt.done.store(true, std::memory_order_release);
  co_return;
}

// Client: connect, send kBigLen bytes via write_all in one call.
Task<void> tcp_client_all(SocketAddr addr, std::atomic<bool> &failed) {
  auto stream = co_await TcpStream::connect(addr);
  if (!stream) { failed.store(true); co_return; }
  std::vector<std::byte> payload(kBigLen, std::byte{0x01});
  auto w = co_await stream->write_all(payload);
  if (!w) failed.store(true);
  co_return;
}

TEST(NetLoopback, TcpWriteAllReadExact) {
  auto addr = SocketAddr::from_ipv4("127.0.0.1", kTcpPort2);
  ASSERT_TRUE(addr.has_value());
  auto listener = TcpListener::bind(*addr);
  ASSERT_TRUE(listener.has_value()) << "bind failed";

  Dispatcher disp(1);
  RoundTrip rt;
  std::atomic<bool> client_failed{false};

  disp.spawn(tcp_server_exact(*listener, rt));
  std::jthread spawn_client([&] {
    std::this_thread::sleep_for(40ms);
    disp.spawn(tcp_client_all(*addr, client_failed));
  });

  const bool completed = run_until(disp, rt.done, 3000ms);
  spawn_client.join();

  ASSERT_TRUE(completed) << "write_all/read_exact round trip timed out";
  EXPECT_FALSE(rt.error.load()) << "read_exact failed";
  EXPECT_FALSE(client_failed.load()) << "write_all failed";
  // Every byte was 0x01, so the checksum must be exactly kBigLen.
  EXPECT_EQ(rt.len.load(), static_cast<size_t>(kBigLen));
}

// ─── Unix domain socket loopback ──────────────────────────────────────────────

const char *const kUnixPath = "/tmp/qbuem_net_loopback_test.sock";

Task<void> unix_server(UnixSocket &listener, RoundTrip &rt) {
  auto client = co_await listener.accept();
  if (!client) {
    rt.error.store(true, std::memory_order_relaxed);
    rt.done.store(true, std::memory_order_release);
    co_return;
  }

  std::array<std::byte, 256> buf{};
  auto n = co_await client->read(buf);
  if (!n || *n == 0) {
    rt.error.store(true, std::memory_order_relaxed);
    rt.done.store(true, std::memory_order_release);
    co_return;
  }

  const size_t got = *n;
  std::memcpy(rt.payload.data(), buf.data(), got);
  rt.len.store(got, std::memory_order_relaxed);
  rt.done.store(true, std::memory_order_release);
  co_return;
}

Task<void> unix_client(std::string_view payload,
                       std::atomic<bool> &write_failed) {
  auto conn = co_await UnixSocket::connect(kUnixPath);
  if (!conn) {
    write_failed.store(true, std::memory_order_relaxed);
    co_return;
  }
  auto w = co_await conn->write(as_bytes_view(payload));
  if (!w || *w != payload.size())
    write_failed.store(true, std::memory_order_relaxed);
  co_return;
}

TEST(NetLoopback, UnixSocketSendAndReadBack) {
  ::unlink(kUnixPath); // remove any stale socket file

  auto listener = UnixSocket::bind(kUnixPath);
  ASSERT_TRUE(listener.has_value()) << "UnixSocket::bind failed";

  constexpr std::string_view kMsg = "unix-domain-loopback-payload-abc";

  Dispatcher disp(1);
  RoundTrip rt;
  std::atomic<bool> client_write_failed{false};

  disp.spawn(unix_server(*listener, rt));

  std::jthread spawn_client([&] {
    std::this_thread::sleep_for(40ms);
    disp.spawn(unix_client(kMsg, client_write_failed));
  });

  const bool completed = run_until(disp, rt.done, 3000ms);
  spawn_client.join();
  ::unlink(kUnixPath);

  ASSERT_TRUE(completed) << "Unix socket round trip timed out";
  EXPECT_FALSE(rt.error.load()) << "server-side accept/read failed";
  EXPECT_FALSE(client_write_failed.load()) << "client connect/write failed";

  const size_t n = rt.len.load();
  EXPECT_EQ(n, kMsg.size());
  EXPECT_EQ(to_string(rt.payload.data(), n), std::string(kMsg));
}

// ─── UDP loopback (send_to / recv_from) ───────────────────────────────────────

constexpr uint16_t kUdpServerPort = 53921;

Task<void> udp_receiver(UdpSocket &sock, RoundTrip &rt) {
  std::array<std::byte, 256> buf{};
  auto r = co_await sock.recv_from(buf);
  if (!r) {
    rt.error.store(true, std::memory_order_relaxed);
    rt.done.store(true, std::memory_order_release);
    co_return;
  }
  auto [n, from] = *r;
  std::memcpy(rt.payload.data(), buf.data(), n);
  rt.len.store(n, std::memory_order_relaxed);
  rt.peer_port.store(from.port(), std::memory_order_relaxed);
  rt.done.store(true, std::memory_order_release);
  co_return;
}

Task<void> udp_sender(SocketAddr dest, std::string_view payload,
                      std::atomic<bool> &send_failed) {
  auto sock = UdpSocket::bind(*SocketAddr::from_ipv4("127.0.0.1", 0));
  if (!sock) {
    send_failed.store(true, std::memory_order_relaxed);
    co_return;
  }
  auto w = co_await sock->send_to(as_bytes_view(payload), dest);
  if (!w || *w != payload.size())
    send_failed.store(true, std::memory_order_relaxed);
  co_return;
}

TEST(NetLoopback, UdpSendToRecvFrom) {
  auto server_addr = SocketAddr::from_ipv4("127.0.0.1", kUdpServerPort);
  ASSERT_TRUE(server_addr.has_value());

  auto recv_sock = UdpSocket::bind(*server_addr);
  ASSERT_TRUE(recv_sock.has_value()) << "UdpSocket::bind failed";

  constexpr std::string_view kMsg = "udp-datagram-loopback-payload-77";

  Dispatcher disp(1);
  RoundTrip rt;
  std::atomic<bool> send_failed{false};

  disp.spawn(udp_receiver(*recv_sock, rt));

  std::jthread spawn_sender([&] {
    std::this_thread::sleep_for(40ms);
    disp.spawn(udp_sender(*server_addr, kMsg, send_failed));
  });

  const bool completed = run_until(disp, rt.done, 3000ms);
  spawn_sender.join();

  ASSERT_TRUE(completed) << "UDP round trip timed out";
  EXPECT_FALSE(rt.error.load()) << "receiver recv_from failed";
  EXPECT_FALSE(send_failed.load()) << "sender bind/send_to failed";

  const size_t n = rt.len.load();
  EXPECT_EQ(n, kMsg.size());
  EXPECT_EQ(to_string(rt.payload.data(), n), std::string(kMsg));
  // The sender bound to an ephemeral port, so the observed source port must be
  // a real, nonzero port assigned by the kernel.
  EXPECT_NE(rt.peer_port.load(), 0u);
}

} // namespace
