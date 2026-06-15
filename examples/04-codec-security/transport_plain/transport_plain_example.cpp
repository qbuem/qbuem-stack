/**
 * @file examples/04-codec-security/transport_plain/transport_plain_example.cpp
 * @brief PlainTransport — the ITransport implementation for un-encrypted TCP,
 *        driven over a real loopback socket by a single-thread Dispatcher.
 *
 * `PlainTransport` is the no-TLS leaf of the `ITransport` abstraction. It owns a
 * non-blocking socket fd and turns the four virtuals (handshake / read / write /
 * close) into reactor-driven coroutines:
 *   - handshake() is a no-op that returns ok() immediately (there is no TLS).
 *   - read()/write() suspend on the AsyncRead / AsyncWrite awaiters, which park
 *     the coroutine on the current thread's Reactor until the fd is ready, then
 *     resume it — never blocking the event loop.
 *   - close() does shutdown(SHUT_WR) + ::close(fd) (sends FIN, half-close).
 *   - negotiated_protocol() is "" because no ALPN happens on a plain socket.
 *
 * This demo wires TWO PlainTransport instances onto the two ends of a single
 * AF_UNIX socketpair (an in-process loopback — no ports, no external network)
 * and runs a tiny request/response exchange entirely through the ITransport
 * interface, on one Reactor thread:
 *
 *     ┌─────────── one Dispatcher worker thread ───────────┐
 *     │                                                    │
 *     │   echo_server(PlainTransport server_t) ──┐         │
 *     │       loop: read → write back (uppercased)│        │
 *     │                                           │        │
 *     │   client(PlainTransport client_t) ────────┘        │
 *     │       handshake → write req → read resp → close    │
 *     └────────────────────────────────────────────────────┘
 *            server_fd  <== socketpair ==>  client_fd
 *
 * Both coroutines are spawned on the SAME single worker (`spawn_on(0, ...)`) so
 * every AsyncRead/AsyncWrite registers against the one Reactor that is polling —
 * a self-contained loopback, exactly like a TCP echo but with no listener.
 *
 * Zero-copy / zero-alloc idioms exercised here:
 *   - read()/write() take std::span<std::byte> / std::span<const std::byte>:
 *     the buffers are stack arrays, passed as views, never copied by value.
 *   - the receive buffer is a single fixed std::array reused every loop turn.
 *   - no std::string is constructed on the I/O path; we view bytes as a
 *     string_view only for the final human-readable print.
 *
 * Build standalone (the mandated self-verify command — note PlainTransport's
 * awaiters need the out-of-line Reactor / Dispatcher / kqueue symbols, so the
 * three src/core reactor sources are #included directly into this TU; the
 * QBUEM_TRANSPORT_PLAIN_LINK_LIB guard makes the project's CMake build skip them
 * to avoid duplicate symbols when libqbuem is linked):
 *
 *   clang++ -std=c++23 -O1 -I <repo>/include \
 *     transport_plain_example.cpp -o /tmp/transport_plain && /tmp/transport_plain
 *
 * NOTE: this example targets POSIX (Linux io_uring / macOS kqueue). The inlined
 * reactor is selected by the same #ifdef ladder the library uses.
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/transport/plain_transport.hpp>

#include <qbuem/compat/print.hpp>

#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <span>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

// ── Out-of-line symbols PlainTransport's awaiters need ──────────────────────────
// AsyncRead/AsyncWrite reach Reactor::current(); Dispatcher owns the Reactor and
// the worker poll loop. These are non-inline (src/core/*.cpp). For the standalone
// compile we pull them straight in. Under the project's CMake (which links the
// library) define QBUEM_TRANSPORT_PLAIN_LINK_LIB so these are skipped.
#ifndef QBUEM_TRANSPORT_PLAIN_LINK_LIB
#include "../../../src/core/reactor.cpp"
#include "../../../src/core/dispatcher.cpp"
#if defined(__APPLE__)
#include "../../../src/core/kqueue_reactor.cpp"
#elif defined(QBUEM_HAS_IOURING)
#include "../../../src/core/io_uring_reactor.cpp"
#else
#include "../../../src/core/epoll_reactor.cpp"
#endif
#endif // QBUEM_TRANSPORT_PLAIN_LINK_LIB

using namespace qbuem;
using std::println;

namespace {

// ─── helpers ──────────────────────────────────────────────────────────────────

void banner(std::string_view title) {
  println("");
  println("┌────────────────────────────────────────────────────────────┐");
  println("│ {:<58} │", title);
  println("└────────────────────────────────────────────────────────────┘");
}

// View bytes as a string_view for printing only — no copy of the I/O buffer.
std::string_view as_text(std::span<const std::byte> b) {
  return {reinterpret_cast<const char *>(b.data()), b.size()};
}

std::span<const std::byte> as_bytes(std::string_view s) {
  return {reinterpret_cast<const std::byte *>(s.data()), s.size()};
}

// Cross-coroutine completion + observed stats. The two coroutines run on the
// same Reactor thread, so plain flags suffice; atomics let main() observe them.
std::atomic<bool>   g_done{false};
std::atomic<size_t> g_server_echoed{0};
std::atomic<size_t> g_client_received{0};
std::atomic<bool>   g_handshake_ok{false};
std::atomic<bool>   g_proto_empty{false};

// ─── echo server side ───────────────────────────────────────────────────────────
// Reads through ITransport, upper-cases the bytes in place (still zero-alloc:
// a single fixed receive buffer is reused), writes them straight back, and
// half-closes when the peer's FIN arrives (read() returns 0 = EOS).

Task<Result<void>> echo_server(PlainTransport transport) {
  std::array<std::byte, 256> buf{};

  for (;;) {
    auto n = co_await transport.read(buf);
    if (!n) {
      println("  [server] read error: {}", n.error().message());
      break;
    }
    if (*n == 0) {
      println("  [server] peer closed (EOS) — shutting down echo side");
      break;
    }

    std::span<std::byte> view{buf.data(), *n};
    // Transform in place — uppercase. No allocation, operating on the live span.
    for (auto &b : view) {
      auto c = static_cast<unsigned char>(b);
      b = static_cast<std::byte>(std::toupper(c));
    }

    println("  [server] read {} byte(s), echoing uppercased: \"{}\"", *n,
            as_text(view));

    auto sent = co_await transport.write(view);
    if (!sent) {
      println("  [server] write error: {}", sent.error().message());
      break;
    }
    g_server_echoed.fetch_add(*sent, std::memory_order_relaxed);
  }

  // Close the server end of the transport (shutdown(SHUT_WR)+close(fd)).
  auto c = co_await transport.close();
  if (!c) println("  [server] close error: {}", c.error().message());
  co_return Result<void>{};
}

// ─── client side ──────────────────────────────────────────────────────────────
// Drives the full ITransport lifecycle: handshake() (no-op for plain), write a
// request, read the echoed response, inspect negotiated_protocol() (empty), then
// close() to send FIN so the server's read() observes EOS.

Task<Result<void>> client(PlainTransport transport) {
  // 1. handshake — plain TCP: returns ok() immediately, no bytes on the wire.
  auto hs = co_await transport.handshake();
  g_handshake_ok.store(hs.has_value(), std::memory_order_relaxed);
  println("  [client] handshake() -> {} (plain TCP no-op)",
          hs ? "ok" : "error");

  // negotiated_protocol() is "" — no ALPN on a plain socket.
  std::string_view proto = transport.negotiated_protocol();
  g_proto_empty.store(proto.empty(), std::memory_order_relaxed);
  println("  [client] negotiated_protocol() -> \"{}\" (empty == no TLS/ALPN)",
          proto);

  // 2. Send a request (stack buffer viewed as bytes — zero copy into write()).
  static constexpr std::string_view request = "hello plain transport";
  println("  [client] write request: \"{}\"", request);
  auto sent = co_await transport.write(as_bytes(request));
  if (!sent) {
    println("  [client] write error: {}", sent.error().message());
    co_return Result<void>{};
  }

  // 3. Read the echoed response into a fixed stack buffer.
  std::array<std::byte, 256> resp{};
  auto n = co_await transport.read(resp);
  if (!n) {
    println("  [client] read error: {}", n.error().message());
    co_return Result<void>{};
  }
  std::span<const std::byte> got{resp.data(), *n};
  g_client_received.store(*n, std::memory_order_relaxed);
  println("  [client] read {} byte(s) back: \"{}\"", *n, as_text(got));

  // 4. Close -> sends FIN; the server's pending read() will return 0 (EOS).
  println("  [client] close() — sending FIN (half-close)");
  auto c = co_await transport.close();
  if (!c) println("  [client] close error: {}", c.error().message());

  g_done.store(true, std::memory_order_release);
  co_return Result<void>{};
}

// Make an fd non-blocking — PlainTransport's contract requires O_NONBLOCK so the
// awaiters get EAGAIN and yield to the Reactor instead of blocking the loop.
bool set_nonblocking(int fd) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

}  // namespace

int main() {
  println("============================================================");
  println(" qbuem-stack · PlainTransport (ITransport, no-TLS)");
  println(" socketpair loopback driven by a single-thread Dispatcher");
  println("============================================================");

  banner("1. Build an in-process loopback (AF_UNIX socketpair)");

  // A socketpair is a connected, bidirectional pipe of sockets — a loopback
  // with no listener, no port, no external network. Two fds: [0] and [1].
  int fds[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    println(stderr, "socketpair() failed");
    return 1;
  }
  if (!set_nonblocking(fds[0]) || !set_nonblocking(fds[1])) {
    println(stderr, "failed to set O_NONBLOCK on socketpair fds");
    ::close(fds[0]);
    ::close(fds[1]);
    return 1;
  }
  const int client_fd = fds[0];
  const int server_fd = fds[1];
  println("socketpair ready: client_fd={}, server_fd={} (both O_NONBLOCK).",
          client_fd, server_fd);
  println("Wrapping each end in a PlainTransport (the no-TLS ITransport leaf).");

  banner("2. Run the exchange through ITransport on one Reactor thread");
  println("Server: read -> uppercase -> write back, until peer FIN (EOS).");
  println("Client: handshake -> write -> read -> close (send FIN).");
  println("");

  // Single-thread Dispatcher: exactly one Reactor, one worker. Both coroutines
  // are spawned on worker 0 so every awaiter parks on the polling Reactor.
  Dispatcher dispatcher(1);
  std::jthread run_th([&] { dispatcher.run(); });

  // PlainTransport takes ownership of the fd lifetime via close(); we move the
  // transports into the coroutines.
  dispatcher.spawn_on(0, [](PlainTransport t) -> Task<void> {
    co_await echo_server(std::move(t));
  }(PlainTransport{server_fd}));

  dispatcher.spawn_on(0, [](PlainTransport t) -> Task<void> {
    co_await client(std::move(t));
  }(PlainTransport{client_fd}));

  // Wait for the client coroutine to signal completion (bounded — never spins
  // forever: a ~2s ceiling so a hang is reported, not hung).
  for (int i = 0; i < 200 && !g_done.load(std::memory_order_acquire); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  dispatcher.stop();
  run_th.join();

  banner("3. Result");
  const bool ok = g_done.load() &&
                  g_handshake_ok.load() &&
                  g_proto_empty.load() &&
                  g_client_received.load() == g_server_echoed.load() &&
                  g_client_received.load() > 0;

  println("handshake() returned ok        : {}", g_handshake_ok.load());
  println("negotiated_protocol() was empty: {}", g_proto_empty.load());
  println("server bytes echoed            : {}", g_server_echoed.load());
  println("client bytes received          : {}", g_client_received.load());
  println("round-trip completed           : {}", g_done.load());
  println("");
  println("{}",
          ok ? "All checks passed — request flowed through PlainTransport, was "
               "echoed (uppercased), and the connection closed cleanly."
             : "FAILED — see the per-step output above.");

  return ok ? 0 : 1;
}
