/**
 * @file examples/tcp_echo_server.cpp
 * @brief TcpListener + TcpStream — asynchronous TCP echo server example.
 */
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/net/socket_addr.hpp>
#include <qbuem/net/tcp_listener.hpp>
#include <qbuem/net/tcp_stream.hpp>

#include <atomic>
#include <array>
#include <chrono>
#include <cstring>
#include <thread>
#include <qbuem/compat/print.hpp>

using namespace qbuem;
using namespace std::chrono_literals;
using std::println;
using std::print;

static std::atomic<size_t> g_connections{0};
static std::atomic<size_t> g_echo_bytes{0};
static std::atomic<bool>   g_stop{false};

// ─── Echo handler coroutine ───────────────────────────────────────────────────

Task<Result<void>> handle_connection(TcpStream stream) {
    g_connections.fetch_add(1, std::memory_order_relaxed);

    std::array<std::byte, 1024> buf{};

    while (!g_stop.load()) {
        // Async read (co_await from the Reactor event loop)
        auto n = co_await stream.read(buf);
        if (!n || *n == 0) break;  // connection closed

        // Echo: send back the received data
        std::span<const std::byte> view{buf.data(), *n};
        auto sent = co_await stream.write(view);
        if (!sent) break;

        g_echo_bytes.fetch_add(*n, std::memory_order_relaxed);
    }
    co_return {};
}

// ─── Accept loop coroutine ────────────────────────────────────────────────────

Task<Result<void>> accept_loop(TcpListener& listener, Dispatcher& dispatcher) {
    while (!g_stop.load()) {
        auto stream = co_await listener.accept();
        if (!stream) break;  // listener closed

        println("[tcp] New connection accepted");

        // Spawn the connection handler as a separate coroutine
        dispatcher.spawn(handle_connection(std::move(*stream)));
    }
    co_return {};
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main() {
    println("=== TCP Echo Server (port 18080) ===");

    // Bind TcpListener
    auto addr = SocketAddr::from_ipv4("127.0.0.1", 18080);
    if (!addr) {
        println(stderr, "[tcp] Invalid address");
        return 1;
    }

    auto listener = TcpListener::bind(*addr);
    if (!listener) {
        println(stderr, "[tcp] bind failed");
        return 1;
    }

    println("[tcp] Listening on 127.0.0.1:18080");
    println("[tcp] (Connect with: nc 127.0.0.1 18080)");
    println("[tcp] Running for 5 seconds then shutting down...");

    Dispatcher dispatcher(1);
    std::jthread run_th([&] { dispatcher.run(); });

    // Spawn accept loop
    dispatcher.spawn(accept_loop(*listener, dispatcher));

    // Auto-shutdown after 5 seconds (example only)
    std::this_thread::sleep_for(5s);

    g_stop.store(true, std::memory_order_release);
    dispatcher.stop();
    run_th.join();

    println("[tcp] Stats: connections={} echo_bytes={}",
            g_connections.load(), g_echo_bytes.load());
    return 0;
}
