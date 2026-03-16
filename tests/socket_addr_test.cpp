/**
 * @file tests/socket_addr_test.cpp
 * @brief Unit tests for SocketAddr (IPv4/IPv6/Unix) and WsFrame opcodes.
 */

#include <gtest/gtest.h>
#include <qbuem/net/socket_addr.hpp>
#include <qbuem/server/websocket_handler.hpp>

#include <cstring>

using namespace qbuem;

// ─── SocketAddr — IPv4 ───────────────────────────────────────────────────────

TEST(SocketAddrTest, FromIpv4ValidAddress) {
    auto addr = SocketAddr::from_ipv4("127.0.0.1", 8080);
    ASSERT_TRUE(addr.has_value());
    EXPECT_EQ(addr->family(), SocketAddr::Family::IPv4);
    EXPECT_EQ(addr->port(), 8080u);
}

TEST(SocketAddrTest, FromIpv4InvalidAddress) {
    auto addr = SocketAddr::from_ipv4("999.999.999.999", 80);
    EXPECT_FALSE(addr.has_value());
}

TEST(SocketAddrTest, FromIpv4ToChars) {
    auto addr = SocketAddr::from_ipv4("127.0.0.1", 18080);
    ASSERT_TRUE(addr.has_value());

    char buf[64];
    int n = addr->to_chars(buf, sizeof(buf));
    EXPECT_GT(n, 0);
    EXPECT_STREQ(buf, "127.0.0.1:18080");
}

TEST(SocketAddrTest, FromIpv4ZeroPort) {
    auto addr = SocketAddr::from_ipv4("0.0.0.0", 0);
    ASSERT_TRUE(addr.has_value());
    EXPECT_EQ(addr->port(), 0u);
}

TEST(SocketAddrTest, FromIpv4ToSockaddr) {
    auto addr = SocketAddr::from_ipv4("127.0.0.1", 9090);
    ASSERT_TRUE(addr.has_value());

    sockaddr_storage ss{};
    socklen_t len{};
    auto r = addr->to_sockaddr(ss, len);
    EXPECT_TRUE(r.has_value());
    EXPECT_EQ(ss.ss_family, AF_INET);
    EXPECT_GT(len, 0u);
}

// ─── SocketAddr — IPv6 ───────────────────────────────────────────────────────

TEST(SocketAddrTest, FromIpv6ValidLoopback) {
    auto addr = SocketAddr::from_ipv6("::1", 8080);
    ASSERT_TRUE(addr.has_value());
    EXPECT_EQ(addr->family(), SocketAddr::Family::IPv6);
    EXPECT_EQ(addr->port(), 8080u);
}

TEST(SocketAddrTest, FromIpv6InvalidAddress) {
    auto addr = SocketAddr::from_ipv6("not-an-ipv6", 80);
    EXPECT_FALSE(addr.has_value());
}

TEST(SocketAddrTest, FromIpv6ToChars) {
    auto addr = SocketAddr::from_ipv6("::1", 9999);
    ASSERT_TRUE(addr.has_value());

    char buf[64];
    int n = addr->to_chars(buf, sizeof(buf));
    EXPECT_GT(n, 0);
    // Should contain "[::1]:9999"
    EXPECT_STREQ(buf, "[::1]:9999");
}

TEST(SocketAddrTest, FromIpv6ToSockaddr) {
    auto addr = SocketAddr::from_ipv6("::1", 1234);
    ASSERT_TRUE(addr.has_value());

    sockaddr_storage ss{};
    socklen_t len{};
    auto r = addr->to_sockaddr(ss, len);
    EXPECT_TRUE(r.has_value());
    EXPECT_EQ(ss.ss_family, AF_INET6);
}

// ─── SocketAddr — Unix ───────────────────────────────────────────────────────

TEST(SocketAddrTest, FromUnixValidPath) {
    auto addr = SocketAddr::from_unix("/tmp/test.sock");
    ASSERT_TRUE(addr.has_value());
    EXPECT_EQ(addr->family(), SocketAddr::Family::Unix);
    EXPECT_EQ(addr->port(), 0u);
}

TEST(SocketAddrTest, FromUnixPathTooLong) {
    // Path > 107 bytes should fail
    std::string long_path(120, 'x');
    long_path = "/" + long_path;
    auto addr = SocketAddr::from_unix(long_path.c_str());
    EXPECT_FALSE(addr.has_value());
}

TEST(SocketAddrTest, FromUnixToChars) {
    auto addr = SocketAddr::from_unix("/tmp/my.sock");
    ASSERT_TRUE(addr.has_value());

    char buf[128];
    int n = addr->to_chars(buf, sizeof(buf));
    EXPECT_GT(n, 0);
    EXPECT_STREQ(buf, "unix:/tmp/my.sock");
}

TEST(SocketAddrTest, FromUnixToSockaddr) {
    auto addr = SocketAddr::from_unix("/tmp/qbuem_test.sock");
    ASSERT_TRUE(addr.has_value());

    sockaddr_storage ss{};
    socklen_t len{};
    auto r = addr->to_sockaddr(ss, len);
    EXPECT_TRUE(r.has_value());
    EXPECT_EQ(ss.ss_family, AF_UNIX);
}

// ─── WsFrame — Opcode values ──────────────────────────────────────────────────

TEST(WsFrameTest, OpcodeValues) {
    EXPECT_EQ(static_cast<uint8_t>(WsFrame::Opcode::Continuation), 0x0);
    EXPECT_EQ(static_cast<uint8_t>(WsFrame::Opcode::Text),         0x1);
    EXPECT_EQ(static_cast<uint8_t>(WsFrame::Opcode::Binary),       0x2);
    EXPECT_EQ(static_cast<uint8_t>(WsFrame::Opcode::Close),        0x8);
    EXPECT_EQ(static_cast<uint8_t>(WsFrame::Opcode::Ping),         0x9);
    EXPECT_EQ(static_cast<uint8_t>(WsFrame::Opcode::Pong),         0xA);
}

TEST(WsFrameTest, DefaultFrameIsFinalTextUnmasked) {
    WsFrame frame;
    EXPECT_EQ(frame.opcode, WsFrame::Opcode::Text);
    EXPECT_TRUE(frame.fin);
    EXPECT_FALSE(frame.masked);
    EXPECT_TRUE(frame.payload.empty());
}

TEST(WsFrameTest, FramePayloadCanBeSet) {
    WsFrame frame;
    frame.opcode = WsFrame::Opcode::Binary;
    frame.payload = {0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_EQ(frame.opcode, WsFrame::Opcode::Binary);
    EXPECT_EQ(frame.payload.size(), 4u);
}

TEST(WsFrameTest, MaskedFlagCanBeSet) {
    WsFrame frame;
    frame.masked = true;
    EXPECT_TRUE(frame.masked);
}
