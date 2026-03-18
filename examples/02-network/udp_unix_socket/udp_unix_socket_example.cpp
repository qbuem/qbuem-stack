/**
 * @file udp_unix_socket_example.cpp
 * @brief UDP socket + Unix domain socket asynchronous I/O example.
 *
 * ## Coverage
 * - UdpSocket::bind()           — bind to address
 * - UdpSocket::send_to()        — async datagram send
 * - UdpSocket::recv_from()      — async datagram receive + sender address
 * - UnixSocket::bind()          — AF_UNIX server socket
 * - UnixSocket::accept()        — async client accept
 * - UnixSocket::connect()       — async client connect
 * - UnixSocket::read() / write()— async stream I/O
 * - SocketAddr::from_ipv4()     — address parsing
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/net/socket_addr.hpp>
#include <qbuem/net/udp_socket.hpp>
#include <qbuem/net/unix_socket.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <cstring>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <qbuem/compat/print.hpp>

using namespace qbuem;
using namespace std::chrono_literals;
using std::println;
using std::print;

// ─────────────────────────────────────────────────────────────────────────────
// §1  UDP echo simulation
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<bool> g_udp_done{false};

static Task<void> udp_receiver(UdpSocket sock) {
    std::array<std::byte, 256> buf{};
    println("[UDP] Waiting for incoming datagram...");

    auto result = co_await sock.recv_from(buf);
    if (!result) {
        println("[UDP] Receive failed: {}", result.error().message());
        co_return;
    }

    auto [n, from] = *result;
    std::string_view msg{reinterpret_cast<const char*>(buf.data()), n};
    println("[UDP] Received: \"{}\" (from port {})", msg, from.port());
    g_udp_done.store(true, std::memory_order_release);
    co_return;
}

static Task<void> udp_sender(SocketAddr dest) {
    auto sock_r = UdpSocket::bind(
        *SocketAddr::from_ipv4("127.0.0.1", 0));  // bind to ephemeral port
    if (!sock_r) {
        println("[UDP] Failed to create sender socket: {}",
                    sock_r.error().message());
        co_return;
    }

    std::string_view payload = "Hello, UDP!";
    auto data = std::as_bytes(std::span(payload.data(), payload.size()));
    auto result = co_await sock_r->send_to(data, dest);
    if (result)
        println("[UDP] Sent: {} bytes", *result);
    else
        println("[UDP] Send failed: {}", result.error().message());
    co_return;
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  Unix domain socket echo server
// ─────────────────────────────────────────────────────────────────────────────

static constexpr const char* kSockPath = "/tmp/qbuem_unix_example.sock";
static std::atomic<bool> g_unix_done{false};

static Task<void> unix_server_handler(UnixSocket client) {
    std::array<std::byte, 128> buf{};
    auto n_r = co_await client.read(buf);
    if (!n_r || *n_r == 0) {
        println("[UDS] Receive failed or EOF");
        co_return;
    }
    size_t n = *n_r;
    std::string_view msg{reinterpret_cast<const char*>(buf.data()), n};
    println("[UDS] Received: \"{}\"", msg);

    // Echo reply
    auto w_r = co_await client.write(std::span(buf.data(), n));
    if (w_r)
        println("[UDS] Echo sent: {} bytes", *w_r);
    co_return;
}

static Task<void> unix_server(UnixSocket server) {
    println("[UDS] Server waiting...");
    auto client_r = co_await server.accept();
    if (!client_r) {
        println("[UDS] accept failed: {}", client_r.error().message());
        co_return;
    }
    co_await unix_server_handler(std::move(*client_r));
    g_unix_done.store(true, std::memory_order_release);
    co_return;
}

static Task<void> unix_client() {
    auto conn_r = co_await UnixSocket::connect(kSockPath);
    if (!conn_r) {
        println("[UDS] connect failed: {}", conn_r.error().message());
        co_return;
    }
    println("[UDS] Connected");

    std::string_view payload = "Hello, Unix!";
    auto data = std::as_bytes(std::span(payload.data(), payload.size()));
    auto w_r = co_await conn_r->write(data);
    if (!w_r) co_return;
    println("[UDS] Sent: \"{}\"", payload);

    // Receive echo
    std::array<std::byte, 128> buf{};
    auto n_r = co_await conn_r->read(buf);
    if (n_r && *n_r > 0) {
        std::string_view echo{reinterpret_cast<const char*>(buf.data()), *n_r};
        println("[UDS] Echo received: \"{}\"", echo);
    }
    co_return;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    println("=== qbuem UDP + Unix Socket Example ===\n");

    // ── UDP ──────────────────────────────────────────────────────────────────
    {
        println("── §1  UDP Echo ──");
        auto bind_addr = SocketAddr::from_ipv4("127.0.0.1", 19876);
        if (!bind_addr) {
            println("Address parse failed");
            return 1;
        }

        auto recv_sock = UdpSocket::bind(*bind_addr);
        if (!recv_sock) {
            println("UDP bind failed: {}", recv_sock.error().message());
            return 1;
        }

        Dispatcher disp(1);
        std::jthread t([&] { disp.run(); });

        disp.spawn(udp_receiver(std::move(*recv_sock)));

        // Brief wait before sending so receiver is ready
        std::this_thread::sleep_for(20ms);
        disp.spawn(udp_sender(*bind_addr));

        auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!g_udp_done.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(10ms);

        disp.stop();
        t.join();
        println("UDP done: {}\n", g_udp_done.load() ? "success" : "timeout");
    }

    // ── Unix Domain Socket ────────────────────────────────────────────────────
    {
        println("── §2  Unix Domain Socket Echo ──");
        ::unlink(kSockPath);  // clean up previous socket file

        auto server_sock = UnixSocket::bind(kSockPath);
        if (!server_sock) {
            println("UDS bind failed: {}", server_sock.error().message());
            return 1;
        }

        Dispatcher disp(2);
        std::jthread t([&] { disp.run(); });

        auto srv = std::move(*server_sock);
        disp.spawn(unix_server(std::move(srv)));

        std::this_thread::sleep_for(20ms);
        disp.spawn(unix_client());

        auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!g_unix_done.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(10ms);

        disp.stop();
        t.join();
        ::unlink(kSockPath);
        println("Unix socket done: {}\n",
                    g_unix_done.load() ? "success" : "timeout");
    }

    println("=== Done ===");
    return 0;
}
