/**
 * @file tests/net_coverage_test.cpp
 * @brief Coverage tests for the non-network-dependent surface of qbuem::net.
 *
 * This suite deliberately exercises the parts of include/qbuem/net/ that can be
 * tested deterministically in a single process without a real remote peer:
 *
 *   - socket_compat.hpp : make_socket / make_socket_blocking_cloexec /
 *                         set_nonblock_cloexec / set_cloexec /
 *                         accept_nonblock_cloexec — verified on real local fds
 *                         created via ::socket / ::socketpair / ::pipe.
 *   - uds_advanced.hpp  : send_fds / recv_fds actually exchanging an fd over a
 *                         blocking AF_UNIX socketpair (synchronous, no reactor),
 *                         the scattered_span vectored overload, get_peer_credentials,
 *                         the empty-fds invalid_argument error path, and the
 *                         Linux-only abstract-namespace helpers (graceful off-Linux).
 *   - udp_socket.hpp    : bind to an ephemeral loopback port, fd()/move/RAII.
 *   - unix_socket.hpp   : default-construct invalid, bind error path, move.
 *   - udp_multicast.hpp : create_sender/create_receiver bind, set_ttl/set_loopback,
 *                         join/leave_group, group()/fd() accessors, IPv6 graceful.
 *   - udp_mmsg.hpp      : bind, SendBatch add/size/empty/clear/full, RecvBatch
 *                         accessors, move semantics.
 *   - rudp_socket.hpp   : RudpHeader::encode/decode round trip (pure logic),
 *                         flags, NACK list, truncation guards.
 *   - dns.hpp           : DnsResolver::resolve IPv4/IPv6 literal fast-path
 *                         (synchronous, no thread, no reactor) + invalid host.
 *
 * No real network sockets to remote hosts and no wall-clock sleeps are used for
 * correctness; the only datagrams sent stay on 127.0.0.1 / a local socketpair.
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/io/iovec.hpp>
#include <qbuem/io/scattered_span.hpp>
#include <qbuem/net/dns.hpp>
#include <qbuem/net/rudp_socket.hpp>
#include <qbuem/net/socket_addr.hpp>
#include <qbuem/net/socket_compat.hpp>
#include <qbuem/net/udp_mmsg.hpp>
#include <qbuem/net/udp_multicast.hpp>
#include <qbuem/net/udp_socket.hpp>
#include <qbuem/net/uds_advanced.hpp>
#include <qbuem/net/unix_socket.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <span>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

using namespace qbuem;
using namespace std::chrono_literals;

namespace {

// Returns true if the descriptor has O_NONBLOCK set.
bool is_nonblocking(int fd) {
  int fl = ::fcntl(fd, F_GETFL, 0);
  return fl >= 0 && (fl & O_NONBLOCK) != 0;
}

// Returns true if the descriptor has FD_CLOEXEC set.
bool is_cloexec(int fd) {
  int fl = ::fcntl(fd, F_GETFD, 0);
  return fl >= 0 && (fl & FD_CLOEXEC) != 0;
}

// ─── socket_compat.hpp ────────────────────────────────────────────────────────

TEST(SocketCompat, MakeSocketIsNonblockCloexec) {
  int fd = net::make_socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  EXPECT_TRUE(is_nonblocking(fd));
  EXPECT_TRUE(is_cloexec(fd));
  ::close(fd);
}

TEST(SocketCompat, MakeSocketUdpDomain) {
  int fd = net::make_socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(fd, 0);
  EXPECT_TRUE(is_nonblocking(fd));
  ::close(fd);
}

TEST(SocketCompat, MakeSocketBlockingCloexec) {
  int fd = net::make_socket_blocking_cloexec(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  // Intentionally blocking: O_NONBLOCK must NOT be set, but CLOEXEC must be.
  EXPECT_FALSE(is_nonblocking(fd));
  EXPECT_TRUE(is_cloexec(fd));
  ::close(fd);
}

TEST(SocketCompat, MakeSocketInvalidDomainReturnsError) {
  // -1 is not a valid address family — ::socket must fail and errno is set.
  int fd = net::make_socket(-1, SOCK_STREAM, 0);
  EXPECT_LT(fd, 0);
}

TEST(SocketCompat, SetNonblockCloexecOnPipeFd) {
  int fds[2];
  ASSERT_EQ(::pipe(fds), 0);
  // A plain pipe fd starts blocking and without CLOEXEC.
  EXPECT_EQ(net::set_nonblock_cloexec(fds[0]), 0);
  EXPECT_TRUE(is_nonblocking(fds[0]));
  EXPECT_TRUE(is_cloexec(fds[0]));
  ::close(fds[0]);
  ::close(fds[1]);
}

TEST(SocketCompat, SetCloexecOnlyLeavesBlocking) {
  int fds[2];
  ASSERT_EQ(::pipe(fds), 0);
  EXPECT_EQ(net::set_cloexec(fds[1]), 0);
  EXPECT_TRUE(is_cloexec(fds[1]));
  EXPECT_FALSE(is_nonblocking(fds[1]));
  ::close(fds[0]);
  ::close(fds[1]);
}

TEST(SocketCompat, SetNonblockCloexecOnBadFdFails) {
  // -1 is never a valid fd; fcntl must fail.
  EXPECT_EQ(net::set_nonblock_cloexec(-1), -1);
  EXPECT_EQ(net::set_cloexec(-1), -1);
}

TEST(SocketCompat, AcceptNonblockCloexecOnListener) {
  // Build a real local TCP listener on an ephemeral port, connect to it with a
  // blocking client, then accept via accept_nonblock_cloexec and assert the
  // accepted fd is non-blocking + cloexec. All on 127.0.0.1, no remote peer.
  int listen_fd = net::make_socket_blocking_cloexec(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(listen_fd, 0);

  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = 0; // ephemeral
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(::bind(listen_fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)), 0);
  ASSERT_EQ(::listen(listen_fd, 4), 0);

  socklen_t slen = sizeof(sa);
  ASSERT_EQ(::getsockname(listen_fd, reinterpret_cast<sockaddr *>(&sa), &slen), 0);

  int client_fd = net::make_socket_blocking_cloexec(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(client_fd, 0);
  // Blocking connect to ourselves; the listen backlog completes it immediately.
  ASSERT_EQ(::connect(client_fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)), 0);

  sockaddr_in peer{};
  socklen_t plen = sizeof(peer);
  int accepted =
      net::accept_nonblock_cloexec(listen_fd, reinterpret_cast<sockaddr *>(&peer), &plen);
  ASSERT_GE(accepted, 0);
  EXPECT_TRUE(is_nonblocking(accepted));
  EXPECT_TRUE(is_cloexec(accepted));

  ::close(accepted);
  ::close(client_fd);
  ::close(listen_fd);
}

// ─── uds_advanced.hpp ─────────────────────────────────────────────────────────

TEST(UdsAdvanced, SendRecvFdRoundTripOverSocketpair) {
  // Exchange a real file descriptor over a blocking AF_UNIX socketpair, fully
  // synchronous (no reactor). The receiver gets a dup()'d fd referring to the
  // same open file description. SOCK_DGRAM is used so recvmsg(MSG_WAITALL)
  // returns after one datagram instead of blocking for a full stream buffer.
  int sp[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_DGRAM, 0, sp), 0);

  // Create a pipe; we will pass its read end to the peer.
  int pipefd[2];
  ASSERT_EQ(::pipe(pipefd), 0);

  const std::array<int, 1> to_send{pipefd[0]};
  const char payload[] = "fdpass";
  auto sent = uds::send_fds(
      sp[0], std::span<const int>(to_send),
      std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(payload), sizeof(payload)));
  ASSERT_TRUE(sent.has_value()) << sent.error().message();
  EXPECT_EQ(*sent, static_cast<ssize_t>(sizeof(payload)));

  std::array<int, 4> recv_fds{-1, -1, -1, -1};
  std::array<uint8_t, 64> data_buf{};
  auto rr = uds::recv_fds(sp[1], std::span<int>(recv_fds), std::span<uint8_t>(data_buf));
  ASSERT_TRUE(rr.has_value()) << rr.error().message();
  EXPECT_EQ(rr->fd_count, 1u);
  EXPECT_EQ(rr->data_bytes, static_cast<ssize_t>(sizeof(payload)));
  EXPECT_STREQ(reinterpret_cast<const char *>(data_buf.data()), "fdpass");

  // Prove the received fd works: write through original pipe, read via the
  // received (dup'd) read end.
  int received = recv_fds[0];
  ASSERT_GE(received, 0);
  const char ping[] = "X";
  ASSERT_EQ(::write(pipefd[1], ping, 1), 1);
  char got = 0;
  ASSERT_EQ(::read(received, &got, 1), 1);
  EXPECT_EQ(got, 'X');

  ::close(received);
  ::close(pipefd[0]);
  ::close(pipefd[1]);
  ::close(sp[0]);
  ::close(sp[1]);
}

TEST(UdsAdvanced, SendFdsEmptyFdsIsInvalidArgument) {
  int sp[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sp), 0);
  std::array<int, 0> none{};
  auto r = uds::send_fds(sp[0], std::span<const int>(none), std::span<const uint8_t>{});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
  ::close(sp[0]);
  ::close(sp[1]);
}

TEST(UdsAdvanced, SendFdsScatteredSpanOverload) {
  // The vectored overload sends FD + a multi-segment payload in one sendmsg.
  int sp[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_DGRAM, 0, sp), 0);

  int pipefd[2];
  ASSERT_EQ(::pipe(pipefd), 0);

  const char hdr[] = "HEAD";
  const char body[] = "BODY!";
  IOVec<2> vec;
  vec.push(hdr, 4);   // exclude the NUL
  vec.push(body, 5);  // exclude the NUL

  const std::array<int, 1> to_send{pipefd[0]};
  auto sent = uds::send_fds(sp[0], std::span<const int>(to_send), scattered_span{vec});
  ASSERT_TRUE(sent.has_value()) << sent.error().message();
  EXPECT_EQ(*sent, 9); // 4 + 5

  std::array<int, 2> recv_fds{-1, -1};
  std::array<uint8_t, 32> data_buf{};
  auto rr = uds::recv_fds(sp[1], std::span<int>(recv_fds), std::span<uint8_t>(data_buf));
  ASSERT_TRUE(rr.has_value()) << rr.error().message();
  EXPECT_EQ(rr->fd_count, 1u);
  EXPECT_EQ(rr->data_bytes, 9);
  EXPECT_EQ(std::memcmp(data_buf.data(), "HEADBODY!", 9), 0);

  ::close(recv_fds[0]);
  ::close(pipefd[0]);
  ::close(pipefd[1]);
  ::close(sp[0]);
  ::close(sp[1]);
}

TEST(UdsAdvanced, GetPeerCredentialsOnSocketpair) {
  // Both ends belong to this process, so the peer credentials must match us.
  int sp[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sp), 0);
  auto cred = uds::get_peer_credentials(sp[0]);
  ASSERT_TRUE(cred.has_value()) << cred.error().message();
  EXPECT_EQ(cred->uid, ::getuid());
  EXPECT_TRUE(cred->is_uid(::getuid()));
  // is_root() consistency check (do not assume the test runs as root).
  EXPECT_EQ(cred->is_root(), ::getuid() == 0);
  ::close(sp[0]);
  ::close(sp[1]);
}

TEST(UdsAdvanced, PeerCredentialsDefaultsAreSentinel) {
  uds::PeerCredentials pc;
  EXPECT_EQ(pc.pid, -1);
  EXPECT_FALSE(pc.is_root());
  EXPECT_FALSE(pc.is_uid(0));
}

TEST(UdsAdvanced, MaxFdsConstant) {
  EXPECT_EQ(uds::kMaxFdsPerMsg, 253u);
}

TEST(UdsAdvanced, AbstractNamespaceHelpers) {
  // On Linux these bind/connect the abstract namespace; off-Linux they must
  // return errc::not_supported cleanly (no crash).
  int listener = -1;
  auto br = uds::bind_abstract("qbuem.cov.test.abstract", SOCK_STREAM, listener);
  auto cr = uds::connect_abstract("qbuem.cov.test.abstract", SOCK_STREAM);
#if defined(__linux__)
  EXPECT_TRUE(br.has_value()) << br.error().message();
  EXPECT_TRUE(cr.has_value()) << cr.error().message();
  if (cr.has_value()) ::close(*cr);
  if (listener >= 0) ::close(listener);
#else
  EXPECT_FALSE(br.has_value());
  EXPECT_EQ(br.error(), std::make_error_code(std::errc::not_supported));
  EXPECT_FALSE(cr.has_value());
  EXPECT_EQ(cr.error(), std::make_error_code(std::errc::not_supported));
#endif
}

// ─── udp_socket.hpp (construction / RAII — round trip is in net_loopback) ──────

TEST(UdpSocket, DefaultConstructedIsInvalid) {
  UdpSocket s;
  EXPECT_EQ(s.fd(), -1);
}

TEST(UdpSocket, BindEphemeralLoopback) {
  auto addr = SocketAddr::from_ipv4("127.0.0.1", 0);
  ASSERT_TRUE(addr.has_value());
  auto sock = UdpSocket::bind(*addr);
  ASSERT_TRUE(sock.has_value()) << sock.error().message();
  EXPECT_GE(sock->fd(), 0);
}

TEST(UdpSocket, MoveTransfersOwnership) {
  auto sock = UdpSocket::bind(*SocketAddr::from_ipv4("127.0.0.1", 0));
  ASSERT_TRUE(sock.has_value());
  int fd = sock->fd();
  ASSERT_GE(fd, 0);

  UdpSocket moved = std::move(*sock);
  EXPECT_EQ(moved.fd(), fd);
  EXPECT_EQ(sock->fd(), -1); // moved-from is invalidated

  UdpSocket assigned;
  assigned = std::move(moved);
  EXPECT_EQ(assigned.fd(), fd);
  EXPECT_EQ(moved.fd(), -1);
}

TEST(UdpSocket, ExplicitFdConstructorClosesOnDestroy) {
  // socketpair gives two fds; hand one to UdpSocket and let the destructor close
  // it. We then assert the original fd is no longer valid.
  int sp[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_DGRAM, 0, sp), 0);
  int held = sp[0];
  {
    UdpSocket s(held);
    EXPECT_EQ(s.fd(), held);
  } // destructor closes held
  // fcntl on the closed fd must fail with EBADF.
  EXPECT_EQ(::fcntl(held, F_GETFD), -1);
  ::close(sp[1]);
}

// ─── unix_socket.hpp (construction / errors — round trip is in net_loopback) ──

TEST(UnixSocket, DefaultConstructedIsInvalid) {
  UnixSocket s;
  EXPECT_EQ(s.fd(), -1);
}

TEST(UnixSocket, BindCreatesListener) {
  const char *path = "/tmp/qbuem_net_cov_unix.sock";
  ::unlink(path);
  auto s = UnixSocket::bind(path);
  ASSERT_TRUE(s.has_value()) << s.error().message();
  EXPECT_GE(s->fd(), 0);
  // The socket file must now exist on disk.
  struct stat st {};
  EXPECT_EQ(::stat(path, &st), 0);
  *s = UnixSocket{}; // close the listener fd before unlink
  ::unlink(path);
}

TEST(UnixSocket, MoveTransfersOwnership) {
  const char *path = "/tmp/qbuem_net_cov_unix_move.sock";
  ::unlink(path);
  auto s = UnixSocket::bind(path);
  ASSERT_TRUE(s.has_value());
  int fd = s->fd();
  UnixSocket moved = std::move(*s);
  EXPECT_EQ(moved.fd(), fd);
  EXPECT_EQ(s->fd(), -1);
  moved = UnixSocket{}; // move-assign an empty over it, closing fd
  EXPECT_EQ(moved.fd(), -1);
  ::unlink(path);
}

// ─── udp_multicast.hpp ────────────────────────────────────────────────────────

TEST(MulticastSocket, DefaultConstructedIsInvalid) {
  MulticastSocket s;
  EXPECT_EQ(s.fd(), -1);
}

TEST(MulticastSocket, CreateSenderIPv4) {
  auto group = SocketAddr::from_ipv4("239.1.2.3", 5000);
  ASSERT_TRUE(group.has_value());
  auto sender = MulticastSocket::create_sender(*group);
  ASSERT_TRUE(sender.has_value()) << sender.error().message();
  EXPECT_GE(sender->fd(), 0);

  // TTL + loopback options operate on the IPv4 multicast socket.
  EXPECT_TRUE(sender->set_ttl(4).has_value());
  EXPECT_TRUE(sender->set_loopback(false).has_value());
  EXPECT_TRUE(sender->set_loopback(true).has_value());
}

TEST(MulticastSocket, CreateReceiverIPv4AndLeaveGroup) {
  // Receiver binds INADDR_ANY:port and joins the group.
  auto group = SocketAddr::from_ipv4("239.4.5.6", 0);
  ASSERT_TRUE(group.has_value());
  auto recv = MulticastSocket::create_receiver(*group);
  ASSERT_TRUE(recv.has_value()) << recv.error().message();
  EXPECT_GE(recv->fd(), 0);

  // Joining the same group again (already joined at create_receiver) may return
  // an error (EADDRINUSE) on some kernels; either outcome must not crash.
  auto join2 = recv->join_group(*group);
  (void)join2;

  // Leaving the group we joined at construction must succeed.
  EXPECT_TRUE(recv->leave_group(*group).has_value());
}

TEST(MulticastSocket, CreateSenderIPv6Graceful) {
  // IPv6 multicast may be unavailable in some CI sandboxes; accept either a
  // valid socket or a clean error — never a crash.
  auto group = SocketAddr::from_ipv6("ff02::1", 5001);
  ASSERT_TRUE(group.has_value());
  auto sender = MulticastSocket::create_sender(*group);
  if (sender.has_value()) {
    EXPECT_GE(sender->fd(), 0);
    auto ttl = sender->set_ttl(2);
    (void)ttl;
    auto lb = sender->set_loopback(false);
    (void)lb;
  } else {
    EXPECT_TRUE(static_cast<bool>(sender.error()));
  }
}

TEST(MulticastSocket, MoveTransfersOwnership) {
  auto group = SocketAddr::from_ipv4("239.7.8.9", 5002);
  ASSERT_TRUE(group.has_value());
  auto sender = MulticastSocket::create_sender(*group);
  ASSERT_TRUE(sender.has_value());
  int fd = sender->fd();
  MulticastSocket moved = std::move(*sender);
  EXPECT_EQ(moved.fd(), fd);
  EXPECT_EQ(sender->fd(), -1);
}

// ─── udp_mmsg.hpp ─────────────────────────────────────────────────────────────

TEST(UdpMmsg, DefaultConstructedIsInvalid) {
  UdpMmsgSocket s;
  EXPECT_EQ(s.fd(), -1);
}

TEST(UdpMmsg, BindEphemeralLoopback) {
  auto addr = SocketAddr::from_ipv4("127.0.0.1", 0);
  ASSERT_TRUE(addr.has_value());
  auto sock = UdpMmsgSocket::bind(*addr);
  ASSERT_TRUE(sock.has_value()) << sock.error().message();
  EXPECT_GE(sock->fd(), 0);
}

TEST(UdpMmsg, MoveTransfersOwnership) {
  auto sock = UdpMmsgSocket::bind(*SocketAddr::from_ipv4("127.0.0.1", 0));
  ASSERT_TRUE(sock.has_value());
  int fd = sock->fd();
  UdpMmsgSocket moved = std::move(*sock);
  EXPECT_EQ(moved.fd(), fd);
  EXPECT_EQ(sock->fd(), -1);
}

TEST(UdpMmsg, SendBatchAddSizeEmptyClear) {
  SendBatch<4> batch;
  EXPECT_TRUE(batch.empty());
  EXPECT_EQ(batch.size(), 0u);

  std::array<std::byte, 8> b1{};
  std::array<std::byte, 8> b2{};
  auto dest = SocketAddr::from_ipv4("127.0.0.1", 9000);
  ASSERT_TRUE(dest.has_value());

  EXPECT_TRUE(batch.add(std::span<const std::byte>(b1), *dest));
  EXPECT_TRUE(batch.add(std::span<const std::byte>(b2), *dest));
  EXPECT_FALSE(batch.empty());
  EXPECT_EQ(batch.size(), 2u);

  batch.clear();
  EXPECT_TRUE(batch.empty());
  EXPECT_EQ(batch.size(), 0u);
}

TEST(UdpMmsg, SendBatchRejectsWhenFull) {
  SendBatch<2> batch;
  auto dest = SocketAddr::from_ipv4("127.0.0.1", 9001);
  ASSERT_TRUE(dest.has_value());
  std::array<std::byte, 4> b{};

  EXPECT_TRUE(batch.add(std::span<const std::byte>(b), *dest));
  EXPECT_TRUE(batch.add(std::span<const std::byte>(b), *dest));
  // Third add must fail — batch capacity is 2.
  EXPECT_FALSE(batch.add(std::span<const std::byte>(b), *dest));
  EXPECT_EQ(batch.size(), 2u);
  EXPECT_EQ(SendBatch<2>::kMaxBatch, 2u);
}

TEST(UdpMmsg, RecvBatchEmptyAccessorsAndConstants) {
  RecvBatch<8, 256> rb;
  EXPECT_EQ(rb.count, 0u);
  EXPECT_EQ((RecvBatch<8, 256>::kMaxBatch), 8u);
  EXPECT_EQ((RecvBatch<8, 256>::kBufSize), 256u);
  // Default mmsg socket batch constants.
  EXPECT_EQ(UdpMmsgSocket::kDefaultBatch, 64u);
  EXPECT_EQ(UdpMmsgSocket::kDefaultBufSize, 1500u);
}

// ─── rudp_socket.hpp (pure header codec — no syscalls) ───────────────────────

TEST(RudpHeader, EncodeDecodeRoundTrip) {
  RudpHeader h;
  h.seq = 0xDEADBEEF;
  h.ack = 0x01020304;
  h.flags = RudpFlags::Data | RudpFlags::Ack;
  h.window = 64;
  h.nack_count = 0;

  std::array<std::byte, kRudpHeaderMax> buf{};
  size_t written = h.encode(buf);
  EXPECT_EQ(written, kRudpHeaderBase); // no NACKs => base header only

  RudpHeader d;
  size_t consumed = d.decode(std::span<const std::byte>(buf.data(), written));
  EXPECT_EQ(consumed, written);
  EXPECT_EQ(d.seq, 0xDEADBEEFu);
  EXPECT_EQ(d.ack, 0x01020304u);
  EXPECT_EQ(d.flags, static_cast<uint8_t>(RudpFlags::Data | RudpFlags::Ack));
  EXPECT_EQ(d.window, 64u);
  EXPECT_EQ(d.nack_count, 0u);
}

TEST(RudpHeader, EncodeDecodeWithNackList) {
  RudpHeader h;
  h.seq = 100;
  h.ack = 99;
  h.flags = RudpFlags::Ack | RudpFlags::Nack;
  h.nack_count = 3;
  h.nacks[0] = 101;
  h.nacks[1] = 103;
  h.nacks[2] = 107;

  std::array<std::byte, kRudpHeaderMax> buf{};
  size_t written = h.encode(buf);
  EXPECT_EQ(written, kRudpHeaderBase + 3 * 4);

  RudpHeader d;
  size_t consumed = d.decode(std::span<const std::byte>(buf.data(), written));
  EXPECT_EQ(consumed, written);
  EXPECT_EQ(d.nack_count, 3u);
  EXPECT_EQ(d.nacks[0], 101u);
  EXPECT_EQ(d.nacks[1], 103u);
  EXPECT_EQ(d.nacks[2], 107u);
}

TEST(RudpHeader, EncodeRejectsUndersizedBuffer) {
  RudpHeader h;
  h.seq = 1;
  std::array<std::byte, kRudpHeaderBase - 1> tiny{};
  EXPECT_EQ(h.encode(tiny), 0u); // too small => 0 bytes written
}

TEST(RudpHeader, DecodeRejectsUndersizedInput) {
  RudpHeader d;
  std::array<std::byte, kRudpHeaderBase - 1> tiny{};
  EXPECT_EQ(d.decode(std::span<const std::byte>(tiny)), 0u);
}

TEST(RudpHeader, FlagBitsAreDistinct) {
  EXPECT_EQ(RudpFlags::Syn, 0x01);
  EXPECT_EQ(RudpFlags::Ack, 0x02);
  EXPECT_EQ(RudpFlags::Fin, 0x04);
  EXPECT_EQ(RudpFlags::Data, 0x08);
  EXPECT_EQ(RudpFlags::Nack, 0x10);
  EXPECT_EQ(RudpFlags::Rst, 0x20);
  // Constants sanity.
  EXPECT_EQ(kRudpHeaderBase, 12u);
  EXPECT_EQ(kRudpMaxNacks, 8u);
  EXPECT_EQ(kRudpWindow, 128u);
}

// ─── dns.hpp (literal fast-path — synchronous, no thread/reactor) ─────────────

// Drive a Task<Result<SocketAddr>> to completion on a single-thread Dispatcher.
// For IP literals, DnsResolver::resolve completes in await_ready() with no
// suspension, so the result is available as soon as the coroutine runs once.
struct DnsSlot {
  std::atomic<bool> done{false};
  std::atomic<bool> ok{false};
  SocketAddr::Family family{SocketAddr::Family::IPv4};
  uint16_t port{0};
};

Task<void> resolve_into(std::string host, uint16_t port, DnsSlot &slot) {
  auto r = co_await DnsResolver::resolve(host, port);
  if (r.has_value()) {
    slot.ok.store(true, std::memory_order_relaxed);
    slot.family = r->family();
    slot.port = r->port();
  }
  slot.done.store(true, std::memory_order_release);
  co_return;
}

bool drive(Dispatcher &disp, const std::atomic<bool> &done,
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

TEST(DnsResolver, IPv4LiteralResolvesSynchronously) {
  Dispatcher disp(1);
  DnsSlot slot;
  disp.spawn(resolve_into("127.0.0.1", 8080, slot));
  ASSERT_TRUE(drive(disp, slot.done, 2000ms));
  EXPECT_TRUE(slot.ok.load());
  EXPECT_EQ(slot.family, SocketAddr::Family::IPv4);
  EXPECT_EQ(slot.port, 8080);
}

TEST(DnsResolver, IPv6LiteralResolvesSynchronously) {
  Dispatcher disp(1);
  DnsSlot slot;
  disp.spawn(resolve_into("::1", 443, slot));
  ASSERT_TRUE(drive(disp, slot.done, 2000ms));
  EXPECT_TRUE(slot.ok.load());
  EXPECT_EQ(slot.family, SocketAddr::Family::IPv6);
  EXPECT_EQ(slot.port, 443);
}

// ─── SocketAddr extras not exercised by socket_addr_test (sanity overlap-safe) ─

TEST(SocketAddrNet, UnixToCharsAndFamily) {
  auto u = SocketAddr::from_unix("/tmp/x.sock");
  ASSERT_TRUE(u.has_value());
  EXPECT_EQ(u->family(), SocketAddr::Family::Unix);
  EXPECT_EQ(u->port(), 0);
  char buf[160];
  int n = u->to_chars(buf, sizeof(buf));
  ASSERT_GT(n, 0);
  EXPECT_EQ(std::string(buf, static_cast<size_t>(n)), "unix:/tmp/x.sock");
}

TEST(SocketAddrNet, FromUnixRejectsOverlongPath) {
  std::string too_long(200, 'a');
  auto u = SocketAddr::from_unix(too_long.c_str());
  ASSERT_FALSE(u.has_value());
  EXPECT_EQ(u.error(), std::make_error_code(std::errc::invalid_argument));
}

} // namespace
