/**
 * @file scatter_send_example.cpp
 * @brief scattered_span zero-copy scatter-gather I/O example.
 *
 * ## Coverage — qbuem/io/scattered_span.hpp
 * - scattered_span construction from IOVec<N>
 * - scattered_span::iov_data() / iov_count() — POSIX writev(2) integration
 * - scattered_span::as_iovec() / implicit span<const iovec> conversion
 * - scattered_span::total_bytes() / size() / empty()
 * - scattered_span::operator[] / front() / back()
 * - scattered_span range iteration (STL range — yields span<const byte>)
 * - scattered_span::subspan()
 * - make_scattered_span() factory
 * - IOVec<N>::as_scattered() shorthand
 *
 * ## Coverage — qbuem/net/uds_advanced.hpp
 * - uds::send_fds(fd, fds, scattered_span) — multi-segment sendmsg payload
 *
 * ## Why scattered_span?
 *
 * Instead of gathering discontiguous buffers into a single heap allocation
 * before writing, scattered_span passes a view of the iovec array directly
 * to the kernel — one syscall, zero allocation, zero copy:
 *
 *   Without: header + body → std::string alloc → write() → 1 syscall, 1 alloc
 *   With:    IOVec<2>{header, body} → writev() → 1 syscall, 0 allocs
 *
 * Http1Handler uses exactly this pattern for every HTTP response.
 */

#include <qbuem/io/scattered_span.hpp>
#include <qbuem/io/iovec.hpp>
#include <qbuem/net/uds_advanced.hpp>
#include <qbuem/compat/print.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

using namespace qbuem;
using std::print;
using std::println;

// Always-on check — unlike assert(), this does NOT disappear with -DNDEBUG.
#define CHECK(expr)                                                      \
    do {                                                                 \
        if (!(expr)) {                                                   \
            std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n",          \
                         #expr, __FILE__, __LINE__);                     \
            std::abort();                                                \
        }                                                                \
    } while (0)

// ─────────────────────────────────────────────────────────────────────────────
// §1  Basic construction and POSIX accessors
// ─────────────────────────────────────────────────────────────────────────────

static void demo_construction() {
    println("── §1  Construction & POSIX accessors ──");

    // Prepare three discontiguous buffers (simulate header, body, trailer)
    static const unsigned char header[]  = {'H', 'D', 'R', ':'};
    static const unsigned char body[]    = {'B', 'O', 'D', 'Y'};
    static const unsigned char trailer[] = {'\r', '\n'};

    // Build iovec array — zero allocation, stack-only
    IOVec<4> vec;
    vec.push(header,  sizeof(header));
    vec.push(body,    sizeof(body));
    vec.push(trailer, sizeof(trailer));

    // Wrap as scattered_span — just a std::span<const iovec> under the hood
    scattered_span s{vec};   // or: vec.as_scattered()

    println("  segments   : {}", s.size());
    println("  total_bytes: {}", s.total_bytes());
    println("  iov_count  : {}", s.iov_count());
    println("  empty      : {}", s.empty() ? "true" : "false");

    // Verify segment sizes and pointer identity (zero-copy guarantee)
    CHECK(s[0].size() == sizeof(header));
    CHECK(s[1].size() == sizeof(body));
    CHECK(s[2].size() == sizeof(trailer));
    CHECK(s.front().data() == reinterpret_cast<const std::byte*>(header));
    CHECK(s.back().data()  == reinterpret_cast<const std::byte*>(trailer));

    println("  [OK] front/back/operator[] correct\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  writev(2) — one syscall for three discontiguous segments
// ─────────────────────────────────────────────────────────────────────────────

static void demo_writev() {
    println("── §2  writev(2) — header + body + trailer in one syscall ──");

    // Simulated HTTP/1.1 response fragments
    static constexpr std::string_view kStatus  = "HTTP/1.1 200 OK\r\n";
    static constexpr std::string_view kHeaders = "Content-Length: 5\r\n\r\n";
    static constexpr std::string_view kBody    = "hello";

    IOVec<3> vec;
    vec.push(kStatus.data(),  kStatus.size());
    vec.push(kHeaders.data(), kHeaders.size());
    vec.push(kBody.data(),    kBody.size());

    scattered_span scatter = vec.as_scattered();

    println("  segments : {} ({} bytes total)",
            scatter.size(), scatter.total_bytes());

    // Open a socket pair — write end uses writev, read end validates
    int sv[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    // ONE writev syscall — kernel gathers from three locations
    ssize_t sent = ::writev(sv[1], scatter.iov_data(), scatter.iov_count());
    CHECK(sent == static_cast<ssize_t>(scatter.total_bytes()));
    println("  writev sent {} bytes", sent);

    // Read back and verify
    std::array<char, 256> buf{};
    ssize_t recvd = ::read(sv[0], buf.data(), buf.size());
    CHECK(recvd == sent);

    std::string_view received{buf.data(), static_cast<std::size_t>(recvd)};
    CHECK(received.starts_with("HTTP/1.1 200 OK"));
    CHECK(received.ends_with("hello"));
    println("  received: '{}'", received);
    println("  [OK] all {} bytes correct\n", recvd);

    ::close(sv[0]);
    ::close(sv[1]);
}

// ─────────────────────────────────────────────────────────────────────────────
// §3  Range iteration — STL range yields span<const byte> per segment
// ─────────────────────────────────────────────────────────────────────────────

static void demo_range_iteration() {
    println("── §3  Range iteration ──");

    static const unsigned char segs[][4] = {
        {0x01, 0x02, 0x03, 0x04},
        {0x05, 0x06, 0x07, 0x08},
        {0x09, 0x0A, 0x0B, 0x0C},
    };

    IOVec<4> vec;
    for (const auto& seg : segs)
        vec.push(seg, sizeof(seg));

    scattered_span s{vec};

    int seg_idx = 0;
    std::size_t total = 0;
    for (std::span<const std::byte> segment : s) {
        print("  seg[{}] ({} bytes):", seg_idx, segment.size());
        for (auto b : segment)
            print(" {:02X}", std::to_integer<unsigned>(b));
        println("");
        total += segment.size();
        ++seg_idx;
    }
    CHECK(total == s.total_bytes());
    println("  [OK] iterated {} segments, {} bytes total\n", seg_idx, total);
}

// ─────────────────────────────────────────────────────────────────────────────
// §4  subspan — take a sub-view without allocation
// ─────────────────────────────────────────────────────────────────────────────

static void demo_subspan() {
    println("── §4  subspan ──");

    static const unsigned char a[] = {0xAA};
    static const unsigned char b[] = {0xBB};
    static const unsigned char c[] = {0xCC};
    static const unsigned char d[] = {0xDD};

    IOVec<4> vec;
    vec.push(a, 1); vec.push(b, 1); vec.push(c, 1); vec.push(d, 1);
    scattered_span full{vec};

    // Take the middle two segments — still zero-allocation
    auto mid = full.subspan(1, 2);
    CHECK(mid.size() == 2u);
    CHECK(std::to_integer<int>(mid[0][0]) == 0xBB);
    CHECK(std::to_integer<int>(mid[1][0]) == 0xCC);
    println("  subspan(1,2) → [{:#04X}, {:#04X}]",
            std::to_integer<unsigned>(mid[0][0]),
            std::to_integer<unsigned>(mid[1][0]));

    // Last half
    auto tail = full.subspan(2);
    CHECK(tail.size() == 2u);
    println("  subspan(2)   → [{:#04X}, {:#04X}]",
            std::to_integer<unsigned>(tail[0][0]),
            std::to_integer<unsigned>(tail[1][0]));

    println("  [OK]\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// §5  make_scattered_span — factory from BufferView pack
// ─────────────────────────────────────────────────────────────────────────────

static void demo_make_scattered_span() {
    println("── §5  make_scattered_span factory ──");

    static const unsigned char p1[] = {1, 2, 3};
    static const unsigned char p2[] = {4, 5};
    static const unsigned char p3[] = {6};

    // Reusable storage — cleared on each call to make_scattered_span
    IOVec<8> storage;
    // Pre-populate to prove clear() is called
    storage.push(p1, sizeof(p1));

    BufferView bv1{p1, sizeof(p1)};
    BufferView bv2{p2, sizeof(p2)};
    BufferView bv3{p3, sizeof(p3)};

    auto s = make_scattered_span(storage, bv1, bv2, bv3);
    CHECK(s.size() == 3u);
    CHECK(s.total_bytes() == 6u);
    CHECK(storage.count == 3u);  // pre-populated entry was cleared

    println("  segments   : {}", s.size());
    println("  total_bytes: {}", s.total_bytes());
    println("  [OK]\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// §6  uds::send_fds with scattered_span — multi-segment sendmsg payload
// ─────────────────────────────────────────────────────────────────────────────

static void demo_uds_send_fds() {
    println("── §6  uds::send_fds(fd, fds, scattered_span) ──");

    // Create a Unix socket pair
    int sv[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    // Create a dummy fd to pass (a pipe read end)
    int pipe_fds[2];
    CHECK(::pipe(pipe_fds) == 0);

    // Multi-segment payload: frame_header + payload_data
    static const unsigned char frame_header[] = {0xCA, 0xFE, 0x00, 0x06};
    static const unsigned char payload_data[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xAB, 0xCD};

    IOVec<2> vec;
    vec.push(frame_header, sizeof(frame_header));
    vec.push(payload_data, sizeof(payload_data));

    // One sendmsg syscall: passes pipe_fds[0] as SCM_RIGHTS + two payload segments
    auto result = uds::send_fds(sv[0], std::span<const int>{&pipe_fds[0], 1},
                                scattered_span{vec});
    if (!result) {
        println("  send_fds error: {}", result.error().message());
    } else {
        println("  send_fds sent {} bytes + 1 fd", *result);

        // Receive the fd and payload
        std::array<unsigned char, 32> recv_buf{};
        int recv_fd = -1;
        std::array<char, CMSG_SPACE(sizeof(int))> cmsg_buf{};

        iovec iov{recv_buf.data(), recv_buf.size()};
        msghdr msg{};
        msg.msg_iov        = &iov;
        msg.msg_iovlen     = 1;
        msg.msg_control    = cmsg_buf.data();
        msg.msg_controllen = cmsg_buf.size();

        ssize_t r = ::recvmsg(sv[1], &msg, 0);
        if (r > 0) {
            cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
            if (cmsg && cmsg->cmsg_type == SCM_RIGHTS) {
                std::memcpy(&recv_fd, CMSG_DATA(cmsg), sizeof(int));
                println("  received fd: {} (valid: {})", recv_fd, recv_fd >= 0 ? "yes" : "no");
            }
            println("  received {} payload bytes", r);
            CHECK(r == static_cast<ssize_t>(sizeof(frame_header) + sizeof(payload_data)));
            CHECK(recv_buf[0] == 0xCA && recv_buf[1] == 0xFE);
            CHECK(recv_buf[4] == 0xDE && recv_buf[5] == 0xAD);
            println("  [OK] payload correct");
            if (recv_fd >= 0) ::close(recv_fd);
        }
    }

    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
    ::close(sv[0]);
    ::close(sv[1]);
    println("");
}

// ─────────────────────────────────────────────────────────────────────────────
// §7  Benchmark: scattered_span vs gather-copy baseline
//
// Uses non-blocking sockets so the benchmark is self-contained — no threads.
// Each round writes header+body; if the socket buffer fills we drain and retry.
// ─────────────────────────────────────────────────────────────────────────────

static void drain_nonblock(int fd) {
    std::array<char, 65536> discard{};
    // Non-blocking: read() returns -1/EAGAIN when empty
    while (::read(fd, discard.data(), discard.size()) > 0) {}
}

static void demo_benchmark() {
    println("── §7  Benchmark: scattered_span vs gather-copy ──");

    static constexpr int kRounds = 20'000;

    std::string header(200, 'H');
    std::string body(1024, 'B');

    // Non-blocking socket pair — writes return EAGAIN when buffer is full
    int sv[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    ::fcntl(sv[0], F_SETFL, O_NONBLOCK);
    ::fcntl(sv[1], F_SETFL, O_NONBLOCK);

    // ── scattered_span path ──────────────────────────────────────────────────
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kRounds; ++i) {
        IOVec<2> vec;
        vec.push(header.data(), header.size());
        vec.push(body.data(),   body.size());
        scattered_span s{vec};
        if (::writev(sv[1], s.iov_data(), s.iov_count()) < 0 && errno == EAGAIN) {
            drain_nonblock(sv[0]);
            ::writev(sv[1], s.iov_data(), s.iov_count());
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    drain_nonblock(sv[0]);

    // ── gather-copy path (baseline) ──────────────────────────────────────────
    auto t2 = std::chrono::steady_clock::now();
    for (int i = 0; i < kRounds; ++i) {
        std::string gathered;
        gathered.reserve(header.size() + body.size());
        gathered.append(header);
        gathered.append(body);
        if (::write(sv[1], gathered.data(), gathered.size()) < 0 && errno == EAGAIN) {
            drain_nonblock(sv[0]);
            ::write(sv[1], gathered.data(), gathered.size());
        }
    }
    auto t3 = std::chrono::steady_clock::now();

    ::close(sv[0]);
    ::close(sv[1]);

    auto us_scatter = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    auto us_gather  = std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();

    println("  scattered_span writev : {} µs ({} ns/op)",
            us_scatter, us_scatter * 1000 / kRounds);
    println("  gather-copy write     : {} µs ({} ns/op)",
            us_gather, us_gather * 1000 / kRounds);
    if (us_scatter > 0)
        println("  ratio (gather/scatter): {:.2f}x",
                static_cast<double>(us_gather) / static_cast<double>(us_scatter));
    println("  [note: includes socket I/O overhead; key benefit is zero heap allocation]\n");
}

int main() {
    println("=== scattered_span zero-copy scatter-gather example ===\n");

    demo_construction();
    demo_writev();
    demo_range_iteration();
    demo_subspan();
    demo_make_scattered_span();
    demo_uds_send_fds();
    demo_benchmark();

    println("=== All sections complete ===");
    return 0;
}
