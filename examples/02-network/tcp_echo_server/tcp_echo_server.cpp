/**
 * @file examples/tcp_echo_server.cpp
 * @brief TcpListener + TcpStream — 비동기 TCP 에코 서버 예시
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
#include <iostream>
#include <thread>

using namespace qbuem;
using namespace std::chrono_literals;

static std::atomic<size_t> g_connections{0};
static std::atomic<size_t> g_echo_bytes{0};
static std::atomic<bool>   g_stop{false};

// ─── 에코 핸들러 코루틴 ──────────────────────────────────────────────────────

Task<Result<void>> handle_connection(TcpStream stream) {
    g_connections.fetch_add(1, std::memory_order_relaxed);

    std::array<std::byte, 1024> buf{};

    while (!g_stop.load()) {
        // 비동기 읽기 (Reactor 이벤트 루프에서 co_await)
        auto n = co_await stream.read(buf);
        if (!n || *n == 0) break;  // 연결 종료

        // 에코: 읽은 데이터를 그대로 전송
        std::span<const std::byte> view{buf.data(), *n};
        auto sent = co_await stream.write(view);
        if (!sent) break;

        g_echo_bytes.fetch_add(*n, std::memory_order_relaxed);
    }
    co_return {};
}

// ─── Accept 루프 코루틴 ──────────────────────────────────────────────────────

Task<Result<void>> accept_loop(TcpListener& listener, Dispatcher& dispatcher) {
    while (!g_stop.load()) {
        auto stream = co_await listener.accept();
        if (!stream) break;  // 리스너 종료

        std::cout << "[tcp] New connection accepted\n";

        // 연결 핸들러를 별도 코루틴으로 spawn
        dispatcher.spawn(handle_connection(std::move(*stream)));
    }
    co_return {};
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== TCP Echo Server (port 18080) ===\n";

    // TcpListener 바인딩
    auto addr = SocketAddr::from_ipv4("127.0.0.1", 18080);
    if (!addr) {
        std::cerr << "[tcp] Invalid address\n";
        return 1;
    }

    auto listener = TcpListener::bind(*addr);
    if (!listener) {
        std::cerr << "[tcp] bind failed\n";
        return 1;
    }

    std::cout << "[tcp] Listening on 127.0.0.1:18080\n";
    std::cout << "[tcp] (Connect with: nc 127.0.0.1 18080)\n";
    std::cout << "[tcp] Running for 5 seconds then shutting down...\n";

    Dispatcher dispatcher(1);
    std::jthread run_th([&] { dispatcher.run(); });

    // Accept 루프 spawn
    dispatcher.spawn(accept_loop(*listener, dispatcher));

    // 5초 후 자동 종료 (예시이므로)
    std::this_thread::sleep_for(5s);

    g_stop.store(true, std::memory_order_release);
    dispatcher.stop();
    run_th.join();

    std::cout << "[tcp] Stats: connections=" << g_connections.load()
              << " echo_bytes=" << g_echo_bytes.load() << "\n";
    return 0;
}
