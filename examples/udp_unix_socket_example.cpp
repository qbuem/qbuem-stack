/**
 * @file udp_unix_socket_example.cpp
 * @brief UDP 소켓 + Unix 도메인 소켓 비동기 I/O 예제.
 *
 * ## 커버리지
 * - UdpSocket::bind()           — 주소에 바인딩
 * - UdpSocket::send_to()        — 비동기 데이터그램 송신
 * - UdpSocket::recv_from()      — 비동기 데이터그램 수신 + 발신자 주소
 * - UnixSocket::bind()          — AF_UNIX 서버 소켓
 * - UnixSocket::accept()        — 비동기 클라이언트 수락
 * - UnixSocket::connect()       — 비동기 클라이언트 연결
 * - UnixSocket::read() / write()— 비동기 스트림 I/O
 * - SocketAddr::from_ipv4()     — 주소 파싱
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/net/socket_addr.hpp>
#include <qbuem/net/udp_socket.hpp>
#include <qbuem/net/unix_socket.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <thread>
#include <unistd.h>

using namespace qbuem;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// §1  UDP 에코 시뮬레이션
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<bool> g_udp_done{false};

static Task<void> udp_receiver(UdpSocket sock) {
    std::array<std::byte, 256> buf{};
    std::printf("[UDP] 수신 대기 시작...\n");

    auto result = co_await sock.recv_from(buf);
    if (!result) {
        std::printf("[UDP] 수신 실패: %s\n", result.error().message().c_str());
        co_return;
    }

    auto [n, from] = *result;
    std::string_view msg{reinterpret_cast<const char*>(buf.data()), n};
    std::printf("[UDP] 수신: \"%.*s\" (from port %u)\n",
                static_cast<int>(msg.size()), msg.data(), from.port());
    g_udp_done.store(true, std::memory_order_release);
    co_return;
}

static Task<void> udp_sender(SocketAddr dest) {
    auto sock_r = UdpSocket::bind(
        *SocketAddr::from_ipv4("127.0.0.1", 0));  // 임의 포트로 바인딩
    if (!sock_r) {
        std::printf("[UDP] 송신 소켓 생성 실패: %s\n",
                    sock_r.error().message().c_str());
        co_return;
    }

    std::string_view payload = "Hello, UDP!";
    auto data = std::as_bytes(std::span(payload.data(), payload.size()));
    auto result = co_await sock_r->send_to(data, dest);
    if (result)
        std::printf("[UDP] 송신 완료: %zu 바이트\n", *result);
    else
        std::printf("[UDP] 송신 실패: %s\n", result.error().message().c_str());
    co_return;
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  Unix 도메인 소켓 에코 서버
// ─────────────────────────────────────────────────────────────────────────────

static constexpr const char* kSockPath = "/tmp/qbuem_unix_example.sock";
static std::atomic<bool> g_unix_done{false};

static Task<void> unix_server_handler(UnixSocket client) {
    std::array<std::byte, 128> buf{};
    auto n_r = co_await client.read(buf);
    if (!n_r || *n_r == 0) {
        std::printf("[UDS] 수신 실패 또는 EOF\n");
        co_return;
    }
    size_t n = *n_r;
    std::string_view msg{reinterpret_cast<const char*>(buf.data()), n};
    std::printf("[UDS] 수신: \"%.*s\"\n", static_cast<int>(n), msg.data());

    // 에코 응답
    auto w_r = co_await client.write(std::span(buf.data(), n));
    if (w_r)
        std::printf("[UDS] 에코 송신: %zu 바이트\n", *w_r);
    co_return;
}

static Task<void> unix_server(UnixSocket server) {
    std::printf("[UDS] 서버 대기 중...\n");
    auto client_r = co_await server.accept();
    if (!client_r) {
        std::printf("[UDS] accept 실패: %s\n",
                    client_r.error().message().c_str());
        co_return;
    }
    co_await unix_server_handler(std::move(*client_r));
    g_unix_done.store(true, std::memory_order_release);
    co_return;
}

static Task<void> unix_client() {
    auto conn_r = co_await UnixSocket::connect(kSockPath);
    if (!conn_r) {
        std::printf("[UDS] connect 실패: %s\n",
                    conn_r.error().message().c_str());
        co_return;
    }
    std::printf("[UDS] 연결 성공\n");

    std::string_view payload = "Hello, Unix!";
    auto data = std::as_bytes(std::span(payload.data(), payload.size()));
    auto w_r = co_await conn_r->write(data);
    if (!w_r) co_return;
    std::printf("[UDS] 송신: \"%s\"\n", std::string(payload).c_str());

    // 에코 수신
    std::array<std::byte, 128> buf{};
    auto n_r = co_await conn_r->read(buf);
    if (n_r && *n_r > 0) {
        std::printf("[UDS] 에코 수신: \"%.*s\"\n",
                    static_cast<int>(*n_r),
                    reinterpret_cast<const char*>(buf.data()));
    }
    co_return;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::printf("=== qbuem UDP + Unix Socket 예제 ===\n\n");

    // ── UDP ─────────────────────────────────────────────────────────────────
    {
        std::printf("── §1  UDP 에코 ──\n");
        auto bind_addr = SocketAddr::from_ipv4("127.0.0.1", 19876);
        if (!bind_addr) {
            std::printf("주소 파싱 실패\n");
            return 1;
        }

        auto recv_sock = UdpSocket::bind(*bind_addr);
        if (!recv_sock) {
            std::printf("UDP 바인딩 실패: %s\n",
                        recv_sock.error().message().c_str());
            return 1;
        }

        Dispatcher disp(1);
        std::thread t([&] { disp.run(); });

        disp.spawn(udp_receiver(std::move(*recv_sock)));

        // 짧은 대기 후 송신 (수신 대기가 먼저 시작되도록)
        std::this_thread::sleep_for(20ms);
        disp.spawn(udp_sender(*bind_addr));

        auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!g_udp_done.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(10ms);

        disp.stop();
        t.join();
        std::printf("UDP 완료: %s\n\n", g_udp_done.load() ? "성공" : "타임아웃");
    }

    // ── Unix Domain Socket ───────────────────────────────────────────────────
    {
        std::printf("── §2  Unix Domain Socket 에코 ──\n");
        ::unlink(kSockPath);  // 이전 소켓 파일 정리

        auto server_sock = UnixSocket::bind(kSockPath);
        if (!server_sock) {
            std::printf("UDS 바인딩 실패: %s\n",
                        server_sock.error().message().c_str());
            return 1;
        }

        Dispatcher disp(2);
        std::thread t([&] { disp.run(); });

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
        std::printf("Unix 소켓 완료: %s\n\n",
                    g_unix_done.load() ? "성공" : "타임아웃");
    }

    std::printf("=== 완료 ===\n");
    return 0;
}
