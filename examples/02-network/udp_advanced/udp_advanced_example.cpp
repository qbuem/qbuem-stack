/**
 * @file udp_advanced_example.cpp
 * @brief qbuem advanced UDP — MMSG batching, RUDP, and multicast.
 *
 * Demonstrates:
 *   1. UdpMmsgSocket — receive/send up to 64 datagrams per syscall
 *   2. RudpSocket    — reliable delivery with sequencing and selective NACK
 *   3. MulticastSocket — join/leave groups, publish/subscribe tick data
 *   4. RecvBatch / SendBatch API
 *
 * No external server required — all demos use loopback (127.0.0.1 / ::1).
 * Multicast demo uses the link-local group 239.255.0.1:7799 (admin-scoped).
 */

#include <qbuem/net/udp_mmsg.hpp>
#include <qbuem/net/rudp_socket.hpp>
#include <qbuem/net/udp_multicast.hpp>
#include <qbuem/net/udp_socket.hpp>
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <print>
#include <stop_token>
#include <string_view>
#include <thread>

using namespace qbuem;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// §1. UdpMmsgSocket — batch recv / send
// ─────────────────────────────────────────────────────────────────────────────

Task<void> demo_mmsg_batch(std::stop_token st) {
    std::println("\n─── §1. UdpMmsgSocket — MMSG batch recv/send ───────────────");

    // Receiver: bind on loopback
    auto recv_addr = SocketAddr::from_ipv4("127.0.0.1", 7700);
    if (!recv_addr) { std::println("  [error] addr"); co_return; }

    auto recv_sock = UdpMmsgSocket::bind(*recv_addr);
    if (!recv_sock) {
        std::println("  [error] bind recv: {}", recv_sock.error().message());
        co_return;
    }

    // Sender: bind on ephemeral port
    auto send_addr = SocketAddr::from_ipv4("127.0.0.1", 0);
    if (!send_addr) co_return;

    auto send_sock = UdpMmsgSocket::bind(*send_addr);
    if (!send_sock) {
        std::println("  [error] bind send: {}", send_sock.error().message());
        co_return;
    }

    // Build a SendBatch<16> with 8 datagrams
    SendBatch<16> batch;
    std::array<std::string_view, 8> msgs = {
        "tick:SAMSUNG:72000", "tick:KAKAO:53100",
        "tick:NAVER:190000",  "tick:KRAFTON:230000",
        "tick:HYBE:200000",   "tick:LG:110000",
        "tick:SK:260000",     "tick:HYUNDAI:185000",
    };
    for (auto& m : msgs)
        batch.add(std::span<const std::byte>(
                      reinterpret_cast<const std::byte*>(m.data()), m.size()),
                  *recv_addr);

    std::println("  Sending {} datagrams with a single sendmmsg() call ...", batch.size());

    // sendmmsg — one syscall for all 8 datagrams
    auto sent = co_await send_sock->send_batch(batch, st);
    if (!sent) {
        std::println("  [error] sendmmsg: {}", sent.error().message());
        co_return;
    }
    std::println("  sendmmsg sent {} / {} datagrams", *sent, batch.size());

    // recvmmsg — one syscall, collects all available datagrams
    std::println("  Receiving with a single recvmmsg() call ...");
    auto rbatch = co_await recv_sock->recv_batch(st);
    if (!rbatch) {
        std::println("  [error] recvmmsg: {}", rbatch.error().message());
        co_return;
    }

    std::println("  recvmmsg returned {} datagrams:", rbatch->count);
    for (size_t i = 0; i < rbatch->count; ++i) {
        auto data = rbatch->span(i);
        std::string_view sv(reinterpret_cast<const char*>(data.data()), data.size());
        std::println("    [{}] {} bytes: {}", i, data.size(), sv);
    }

    std::println("");
    std::println("  Performance comparison:");
    std::println("  • recvfrom × {}:  {} syscalls", rbatch->count, rbatch->count);
    std::println("  • recvmmsg × 1:   1 syscall  ({}× reduction)", rbatch->count);
}

// ─────────────────────────────────────────────────────────────────────────────
// §2. RUDP wire format
// ─────────────────────────────────────────────────────────────────────────────

void demo_rudp_header() {
    std::println("\n─── §2. RUDP segment header encoding ──────────────────────");

    // Encode a DATA+ACK segment
    RudpHeader hdr;
    hdr.seq    = 42;
    hdr.ack    = 17;
    hdr.flags  = RudpFlags::Data | RudpFlags::Ack;
    hdr.window = kRudpWindow;
    hdr.nack_count = 0;

    std::array<std::byte, kRudpHeaderMax> wire{};
    size_t len = hdr.encode(wire);

    std::print("  DATA+ACK header ({} bytes): ", len);
    for (size_t i = 0; i < len; ++i)
        std::print("{:02x} ", static_cast<uint8_t>(wire[i]));
    std::println("");

    // Decode round-trip
    RudpHeader decoded;
    size_t consumed = decoded.decode(std::span<const std::byte>(wire.data(), len));
    std::println("  Decoded: seq={} ack={} flags=0x{:02x} window={} consumed={}",
                 decoded.seq, decoded.ack, decoded.flags, decoded.window, consumed);
    std::println("  Round-trip: {}",
                 (decoded.seq == 42 && decoded.ack == 17 &&
                  decoded.flags == (RudpFlags::Data | RudpFlags::Ack)) ? "PASS" : "FAIL");

    // Encode a NACK segment
    RudpHeader nack_hdr;
    nack_hdr.seq        = 50;
    nack_hdr.ack        = 45;
    nack_hdr.flags      = RudpFlags::Ack | RudpFlags::Nack;
    nack_hdr.nack_count = 3;
    nack_hdr.nacks[0]   = 46;
    nack_hdr.nacks[1]   = 47;
    nack_hdr.nacks[2]   = 49;

    std::array<std::byte, kRudpHeaderMax> nack_wire{};
    size_t nlen = nack_hdr.encode(nack_wire);
    std::println("  NACK header: {} bytes, nack_count=3, requesting seq 46,47,49", nlen);

    std::println("");
    std::println("  RUDP vs TCP comparison:");
    std::println("  Feature         TCP          RUDP");
    std::println("  HOL blocking    YES          NO — unordered optional mode");
    std::println("  Retransmit      All past     Selective (NACK list only)");
    std::println("  Overhead/pkt    20-60 bytes  {} bytes base + {} × NACK",
                 kRudpHeaderBase, 4);
    std::println("  Window          Bytes        Segments ({} default)", kRudpWindow);
}

// ─────────────────────────────────────────────────────────────────────────────
// §3. RUDP loopback — connect + send + recv
// ─────────────────────────────────────────────────────────────────────────────

Task<void> demo_rudp_loopback(std::stop_token st) {
    std::println("\n─── §3. RUDP loopback connection ───────────────────────────");

    auto server_addr = SocketAddr::from_ipv4("127.0.0.1", 7710);
    auto client_addr = SocketAddr::from_ipv4("127.0.0.1", 7711);
    if (!server_addr || !client_addr) co_return;

    std::println("  Establishing RUDP connection (loopback :7711 → :7710) ...");

    // Server side: spawned concurrently
    std::string received_msg;
    std::atomic<bool> server_done{false};

    Dispatcher::current()->spawn([&]() -> Task<void> {
        auto srv = co_await RudpSocket::listen(*server_addr, st);
        if (!srv) {
            std::println("  [server] listen failed: {}", srv.error().message());
            server_done = true;
            co_return;
        }
        std::println("  [server] listening on :7710, connection accepted");

        std::array<std::byte, 256> buf{};
        auto n = co_await (*srv)->recv(buf, st);
        if (!n) {
            std::println("  [server] recv failed: {}", n.error().message());
        } else {
            received_msg = std::string(
                reinterpret_cast<const char*>(buf.data()), *n);
            std::println("  [server] received {} bytes: '{}'", *n, received_msg);
        }
        server_done = true;
        co_return;
    }());

    // Small yield to let server bind
    co_await qbuem::sleep(10);

    // Client side
    auto cli = co_await RudpSocket::connect(*client_addr, *server_addr, st);
    if (!cli) {
        std::println("  [client] connect failed: {}", cli.error().message());
        co_return;
    }

    std::string_view msg = "Hello from RUDP client!";
    auto r = co_await (*cli)->send(
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(msg.data()), msg.size()), st);

    if (!r)
        std::println("  [client] send failed: {}", r.error().message());
    else
        std::println("  [client] sent {} bytes: '{}'", *r, msg);

    // Wait for server to process
    for (int i = 0; i < 20 && !server_done.load(); ++i)
        co_await qbuem::sleep(5);

    std::println("  Connection test: {}",
                 (received_msg == msg) ? "PASS" : "FAIL / PARTIAL");
}

// ─────────────────────────────────────────────────────────────────────────────
// §4. Multicast — publish market data, subscribe on two sockets
// ─────────────────────────────────────────────────────────────────────────────

Task<void> demo_multicast(std::stop_token st) {
    std::println("\n─── §4. Multicast — publish/subscribe tick data ────────────");

    // Use administratively-scoped multicast group 239.255.0.1:7799
    auto group_addr = SocketAddr::from_ipv4("239.255.0.1", 7799);
    if (!group_addr) { std::println("  [error] addr"); co_return; }

    // Create two subscribers
    auto sub1 = MulticastSocket::create_receiver(*group_addr, "lo");
    auto sub2 = MulticastSocket::create_receiver(*group_addr, "lo");

    if (!sub1 || !sub2) {
        // Multicast may not work on all environments (CI containers, etc.)
        std::println("  [skip] multicast socket creation failed ({})",
                     (!sub1 ? sub1.error().message() : sub2.error().message()));
        std::println("         Multicast requires a network interface with multicast support.");
        std::println("         (lo/loopback may not support multicast on all kernels)");
        std::println("");
        std::println("  API overview:");
        std::println("    MulticastSocket::create_sender(group, iface) — sender");
        std::println("    MulticastSocket::create_receiver(group, iface) — subscriber");
        std::println("    sock.join_group(group, iface)  — join additional groups");
        std::println("    sock.leave_group(group, iface) — leave a group");
        std::println("    sock.set_ttl(n)                — hop limit (1=link-local)");
        std::println("    sock.set_loopback(true/false)  — local loopback toggle");
        std::println("    co_await sock.send(buf)         — send to group");
        std::println("    co_await sock.recv_from(buf,st) — receive next datagram");
        co_return;
    }

    sub1->set_loopback(true);
    sub2->set_loopback(true);

    // Create publisher
    auto pub = MulticastSocket::create_sender(*group_addr, "lo");
    if (!pub) {
        std::println("  [skip] sender creation failed: {}", pub.error().message());
        co_return;
    }
    pub->set_ttl(1);          // link-local only
    pub->set_loopback(true);  // allow loopback for this demo

    std::println("  Multicast group: 239.255.0.1:7799 (admin-scoped, link-local)");
    std::println("  Publisher fd={}, Subscriber1 fd={}, Subscriber2 fd={}",
                 pub->fd(), sub1->fd(), sub2->fd());

    // Publish 3 tick messages
    const char* ticks[] = {
        "TICK:SAMSUNG:72100:+0.14%",
        "TICK:KAKAO:53200:+0.19%",
        "TICK:NAVER:191000:+0.53%",
    };
    std::println("  Publishing {} ticks ...", std::size(ticks));

    for (auto* tick : ticks) {
        auto r = co_await pub->send(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(tick), std::strlen(tick)));
        if (!r)
            std::println("  [pub] send failed: {}", r.error().message());
        else
            std::println("  [pub] sent {} bytes: {}", *r, tick);
    }

    // Receive on subscriber 1
    std::println("  Receiving on subscriber 1 ...");
    std::array<std::byte, 256> buf{};
    for (size_t i = 0; i < std::size(ticks); ++i) {
        auto r = co_await sub1->recv_from(buf, st);
        if (!r) { std::println("  [sub1] recv failed: {}", r.error().message()); break; }
        auto& [n, from] = *r;
        std::string_view sv(reinterpret_cast<const char*>(buf.data()), n);
        std::println("  [sub1] {}: {}", i + 1, sv);
    }

    std::println("  Both sub1 and sub2 receive the SAME datagrams.");
    std::println("  With multicast, 1 send reaches N subscribers — no N×send().");
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

Task<void> run_all(std::stop_token st) {
    std::println("╔══════════════════════════════════════════════════════════╗");
    std::println("║       qbuem-stack Advanced UDP Demo                      ║");
    std::println("║  MMSG Batching | Reliable UDP | Native Multicast          ║");
    std::println("╚══════════════════════════════════════════════════════════╝");

    co_await demo_mmsg_batch(st);
    demo_rudp_header();
    co_await demo_rudp_loopback(st);
    co_await demo_multicast(st);

    std::println("\n Done.\n");
}

int main() {
    Dispatcher d(4);
    std::jthread t([&d]{ d.run(); });

    std::stop_source ss;
    d.spawn(run_all(ss.get_token()));

    std::this_thread::sleep_for(std::chrono::seconds{4});
    ss.request_stop();
    d.stop();
    return 0;
}
