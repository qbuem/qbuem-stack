/**
 * @file io_coverage_test.cpp
 * @brief Coverage tests for under-tested qbuem/io/ modules.
 *
 * Focuses on surfaces NOT covered by io_buf_test / io_buffers_test /
 * scattered_span_test / scatter_gather_test:
 *   - zero_copy.hpp   : sendfile a /tmp file to a socketpair, splice /
 *     send_zerocopy not_supported / graceful on non-Linux.
 *   - socket_opts.hpp : graceful not_supported / system error returns for
 *     every setsockopt helper, set_reuseaddr success on a real socket.
 *   - read_buf.hpp / write_buf.hpp / iovec.hpp / io_slice.hpp : additional
 *     edge cases (compact no-op, partial consume cycles, append round-trip,
 *     iovec total_bytes after clear) not asserted elsewhere.
 *
 * Note: direct_file.hpp depends on the Linux-only O_DIRECT flag and does not
 * compile on macOS, so it is intentionally excluded here.
 *
 * All tests are deterministic, single-process. The socketpair used by the
 * sendfile test is a connected AF_UNIX pair (no external network).
 */

// System headers first so that socket_opts.hpp (which only pulls socket
// headers under __linux__) sees the POSIX socket symbols on macOS too.
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <qbuem/io/io_slice.hpp>
#include <qbuem/io/iovec.hpp>
#include <qbuem/io/read_buf.hpp>
#include <qbuem/io/scattered_span.hpp>
#include <qbuem/io/socket_opts.hpp>
#include <qbuem/io/write_buf.hpp>
#include <qbuem/io/zero_copy.hpp>

#include <array>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

using namespace qbuem;

// ─── Synchronous coroutine driver ─────────────────────────────────────────────
//
// On a thread with no active Reactor, the zero_copy awaiters perform their
// syscall inline and resume immediately, so the Task completes within a single
// resume(). This helper co_awaits a Task<Result<T>> and captures the value.

template <typename T>
struct CaptureResult {
  std::optional<Result<T>> out;
};

template <typename T>
static Task<void> drive_into(Task<Result<T>> inner, CaptureResult<T>* cap) {
  cap->out = co_await std::move(inner);
}

template <typename T>
static Result<T> run_sync(Task<Result<T>> task) {
  CaptureResult<T> cap;
  Task<void> driver = drive_into<T>(std::move(task), &cap);
  // Resume until the driver coroutine finishes.
  while (driver.resume()) { /* keep resuming */ }
  return std::move(*cap.out);
}

// ─── zero_copy::sendfile ──────────────────────────────────────────────────────

TEST(ZeroCopyTest, SendfileToSocketpairOrNotSupported) {
  // Prepare a temp file with known content.
  const char* path = "/tmp/qbuem_io_sendfile_src.bin";
  ::unlink(path);
  const char payload[] = "ZEROCOPY-SENDFILE-PAYLOAD-0123456789";
  const size_t plen = sizeof(payload) - 1;
  int in_fd = ::open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  ASSERT_GE(in_fd, 0);
  ASSERT_EQ(::write(in_fd, payload, plen), static_cast<ssize_t>(plen));

  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  // sv[0] is the destination socket; sv[1] is the receiver.
  auto res = run_sync<size_t>(
      zero_copy::sendfile(sv[0], in_fd, /*offset=*/0, /*count=*/plen));

  if (res) {
    EXPECT_GT(*res, 0u);
    EXPECT_LE(*res, plen);
    // Read back what was transferred and verify it is a prefix of payload.
    std::array<char, 64> rb{};
    ssize_t got = ::recv(sv[1], rb.data(), *res, 0);
    EXPECT_EQ(got, static_cast<ssize_t>(*res));
    EXPECT_EQ(std::memcmp(rb.data(), payload, static_cast<size_t>(got)), 0);
  } else {
    // Graceful failure: an error_code, never a crash/exception.
    EXPECT_NE(res.error().value(), 0);
  }

  ::close(sv[0]);
  ::close(sv[1]);
  ::close(in_fd);
  ::unlink(path);
}

TEST(ZeroCopyTest, SpliceGracefulOnNonLinux) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  // On non-Linux this returns function_not_supported without touching fds.
  // On Linux it may succeed/fail depending on data availability; either way
  // it must return a Result, never crash.
  auto res = run_sync<size_t>(zero_copy::splice(sv[1], sv[0], 16));
  if (!res) {
    EXPECT_NE(res.error().value(), 0);
  } else {
    EXPECT_GE(*res, 0u);
  }
  ::close(sv[0]);
  ::close(sv[1]);
}

TEST(ZeroCopyTest, SendZerocopyGracefulPath) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  const char data[] = "zc";
  auto res = run_sync<size_t>(
      zero_copy::send_zerocopy(sv[0], data, sizeof(data) - 1));
  // Non-Linux: not_supported. Linux without SO_ZEROCOPY set on AF_UNIX:
  // likely an error. Must be a clean Result either way.
  if (!res)
    EXPECT_NE(res.error().value(), 0);
  else
    EXPECT_LE(*res, sizeof(data) - 1);
  ::close(sv[0]);
  ::close(sv[1]);
}

// ─── socket_opts ──────────────────────────────────────────────────────────────

TEST(SocketOptsTest, SetReuseaddrSucceedsOnRealSocket) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  auto r = io::set_reuseaddr(fd);
  EXPECT_TRUE(r) << r.error().message();
  ::close(fd);
}

TEST(SocketOptsTest, SetReuseportPortableResult) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  auto r = io::set_reuseport(fd);
  // Linux/macOS support SO_REUSEPORT; elsewhere not_supported. No crash.
  if (!r)
    EXPECT_NE(r.error().value(), 0);
  ::close(fd);
}

TEST(SocketOptsTest, SetIncomingCpuGraceful) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  auto r = io::set_incoming_cpu(fd, 0);
  // macOS / old kernels -> not_supported; modern Linux -> success.
  if (!r)
    EXPECT_NE(r.error().value(), 0);
  ::close(fd);
}

TEST(SocketOptsTest, EnableTcpMigrateReqGraceful) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  auto r = io::enable_tcp_migrate_req(fd);
  if (!r)
    EXPECT_NE(r.error().value(), 0);
  ::close(fd);
}

TEST(SocketOptsTest, SetTcpFastopenGraceful) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  auto r = io::set_tcp_fastopen(fd, 16);
  if (!r)
    EXPECT_NE(r.error().value(), 0);
  ::close(fd);
}

TEST(SocketOptsTest, SetZerocopyGraceful) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  auto r = io::set_zerocopy(fd);
  if (!r)
    EXPECT_NE(r.error().value(), 0);
  ::close(fd);
}

TEST(SocketOptsTest, SetReuseportCbpfGraceful) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  auto r = io::set_reuseport_cbpf(fd, 4);
  // Requires Linux + BPF_LD; otherwise not_supported. Must not crash.
  if (!r)
    EXPECT_NE(r.error().value(), 0);
  ::close(fd);
}

TEST(SocketOptsTest, OptsOnInvalidFdReturnError) {
  // A closed/invalid fd should yield an error_code (EBADF or not_supported on
  // platforms that short-circuit), never UB.
  int bad = -1;
  auto r = io::set_reuseaddr(bad);
  EXPECT_FALSE(r);
  EXPECT_NE(r.error().value(), 0);
}

// ─── ReadBuf additional edge cases ────────────────────────────────────────────

TEST(ReadBufExtraTest, CompactIsNoOpWhenReadPosZero) {
  ReadBuf<16> buf;
  std::memcpy(buf.write_head(), "abcd", 4);
  buf.commit(4);
  // read_pos still 0 -> compact() is a no-op, data untouched.
  buf.compact();
  EXPECT_EQ(buf.size(), 4u);
  EXPECT_EQ(buf.read_pos, 0u);
  EXPECT_EQ(buf.write_pos, 4u);
  auto v = buf.readable();
  EXPECT_EQ(std::memcmp(v.data(), "abcd", 4), 0);
}

TEST(ReadBufExtraTest, CompactReclaimsAfterPartialConsume) {
  ReadBuf<8> buf;
  std::memcpy(buf.write_head(), "12345678", 8);
  buf.commit(8);
  EXPECT_TRUE(buf.full());
  buf.consume(5);                 // 3 bytes remain: "678"
  EXPECT_EQ(buf.size(), 3u);
  buf.compact();                  // move "678" to the front
  EXPECT_EQ(buf.read_pos, 0u);
  EXPECT_EQ(buf.write_pos, 3u);
  EXPECT_EQ(buf.writable_size(), 5u);
  auto v = buf.readable();
  EXPECT_EQ(std::memcmp(v.data(), "678", 3), 0);
}

TEST(ReadBufExtraTest, FullConsumeThenEmpty) {
  ReadBuf<4> buf;
  std::memcpy(buf.write_head(), "wxyz", 4);
  buf.commit(4);
  buf.consume(4);
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(buf.size(), 0u);
  // readable() now spans zero bytes from read_pos == write_pos.
  EXPECT_EQ(buf.readable().size(), 0u);
}

TEST(ReadBufExtraTest, WriteHeadAdvancesWithWritePos) {
  ReadBuf<32> buf;
  std::byte* base = buf.write_head();
  buf.commit(10);
  EXPECT_EQ(buf.write_head(), base + 10);
  EXPECT_EQ(buf.writable_size(), 22u);
}

// ─── WriteBuf additional edge cases ───────────────────────────────────────────

TEST(WriteBufExtraTest, AppendRawPointerThenIovecPointsAtSame) {
  WriteBuf wb(64);
  const char* msg = "hello-raw";
  wb.append(static_cast<const void*>(msg), 9);
  EXPECT_EQ(wb.size(), 9u);
  auto iov = wb.as_iovec();
  ASSERT_EQ(iov.count, 1u);
  EXPECT_EQ(iov.vecs[0].iov_len, 9u);
  EXPECT_EQ(std::memcmp(iov.vecs[0].iov_base, msg, 9), 0);
}

TEST(WriteBufExtraTest, ClearKeepsBufferReusable) {
  WriteBuf wb;
  wb.append(std::string_view{"first"});
  EXPECT_FALSE(wb.empty());
  wb.clear();
  EXPECT_TRUE(wb.empty());
  EXPECT_EQ(wb.size(), 0u);
  EXPECT_TRUE(wb.as_iovec().empty());
  // Reusable after clear.
  wb.append(std::string_view{"second-longer"});
  EXPECT_EQ(wb.size(), 13u);
}

TEST(WriteBufExtraTest, MixedAppendOverloadsConcatenate) {
  WriteBuf wb;
  std::array<uint8_t, 3> bytes{{0xAA, 0xBB, 0xCC}};
  wb.append(BufferView{bytes.data(), bytes.size()});
  wb.append(std::string_view{"X"});
  const char raw[2] = {'Y', 'Z'};
  wb.append(static_cast<const void*>(raw), 2);
  EXPECT_EQ(wb.size(), 6u);
  auto iov = wb.as_iovec();
  ASSERT_EQ(iov.count, 1u);
  const auto* p = static_cast<const uint8_t*>(iov.vecs[0].iov_base);
  EXPECT_EQ(p[0], 0xAA);
  EXPECT_EQ(p[3], static_cast<uint8_t>('X'));
  EXPECT_EQ(p[5], static_cast<uint8_t>('Z'));
}

// ─── IOVec additional edge cases ──────────────────────────────────────────────

TEST(IOVecExtraTest, TotalBytesIsZeroAfterClear) {
  IOVec<4> vec;
  int a = 0, b = 0;
  vec.push(&a, sizeof(a));
  vec.push(&b, sizeof(b));
  EXPECT_EQ(vec.total_bytes(), 2 * sizeof(int));
  vec.clear();
  EXPECT_TRUE(vec.empty());
  EXPECT_EQ(vec.total_bytes(), 0u);
  EXPECT_FALSE(vec.full());
}

TEST(IOVecExtraTest, AsConstSpanMatchesCount) {
  IOVec<3> vec;
  char x = 'x';
  vec.push(&x, 1);
  auto cspan = vec.as_const_span();
  EXPECT_EQ(cspan.size(), 1u);
  EXPECT_EQ(cspan[0].iov_len, 1u);
  EXPECT_EQ(cspan[0].iov_base, &x);
}

TEST(IOVecExtraTest, AsScatteredReflectsEntries) {
  IOVec<2> vec;
  const char h[] = "HEAD";
  const char b[] = "BODY!!";
  vec.push(h, 4);
  vec.push(b, 6);
  auto scatter = vec.as_scattered();
  EXPECT_EQ(scatter.size(), 2u);
  EXPECT_EQ(scatter.iov_count(), 2);
  EXPECT_EQ(scatter.total_bytes(), 10u);
  EXPECT_EQ(scatter.front().size(), 4u);
  EXPECT_EQ(scatter.back().size(), 6u);
}

// ─── IOSlice / MutableIOSlice additional edge cases ───────────────────────────

TEST(IOSliceExtraTest, EmptySliceConvertsCleanly) {
  IOSlice s{nullptr, 0};
  auto bv = s.to_buffer_view();
  EXPECT_EQ(bv.size(), 0u);
  auto iov = s.to_iovec();
  EXPECT_EQ(iov.iov_len, 0u);
}

TEST(IOSliceExtraTest, MutableSliceWriteThenReadThroughView) {
  std::array<std::byte, 8> storage{};
  MutableIOSlice ms{storage.data(), storage.size()};
  auto wv = ms.to_buffer_view();    // span<uint8_t>
  ASSERT_EQ(wv.size(), 8u);
  wv[0] = 0x42;
  wv[7] = 0x99;
  // Read back through a const slice over the same storage.
  IOSlice cs = ms.as_const();
  auto rv = cs.to_buffer_view();
  EXPECT_EQ(rv[0], 0x42);
  EXPECT_EQ(rv[7], 0x99);
}

TEST(IOSliceExtraTest, IovecBaseRoundTripsToOriginalPointer) {
  std::array<std::byte, 4> storage{};
  MutableIOSlice ms{storage.data(), storage.size()};
  iovec iv = ms.to_iovec();
  EXPECT_EQ(iv.iov_base, storage.data());
  EXPECT_EQ(iv.iov_len, 4u);
}
