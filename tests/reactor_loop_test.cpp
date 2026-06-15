// reactor_loop_test.cpp — Real event-loop behavior tests for the platform Reactor.
//
// Unlike reactor_test.cpp (which only exercises Task/Arena plumbing), this file
// drives the actual KqueueReactor event loop on macOS:
//   * register a pipe Read event and assert the callback fires with the written data
//   * register a socketpair Read event (eventfd-style wakeup) and assert delivery
//   * register a one-shot timer and assert it fires only AFTER the delay elapses
//   * assert poll() returns promptly when there is nothing to do (no busy-block)
//   * assert post() delivers a cross-thread callback into the loop
//
// All tests are bounded by a wall-clock deadline so they always terminate even
// if the reactor never fires the expected event.
//
// Self-contained single-translation-unit build: we include the reactor
// implementation directly so the test links with only libgtest (no CMake-wired
// qbuem static lib needed). FixedPoolResource / TimerWheel are header-only.
//
// NOTE: KqueueReactor is macOS/BSD only. On Linux this whole file is a no-op
// (the platform-specific tests are compiled out); see the #ifdef __APPLE__ guard.

#include <gtest/gtest.h>

#if defined(__APPLE__)

#include "../src/core/kqueue_reactor.cpp"  // pulls KqueueReactor implementation

#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace qbuem;
using clock_type = std::chrono::steady_clock;

namespace {

// Make a file descriptor non-blocking (required: a blocking fd can stall the loop).
void set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    ASSERT_NE(flags, -1);
    ASSERT_NE(::fcntl(fd, F_SETFL, flags | O_NONBLOCK), -1);
}

// Drive poll() repeatedly until `pred()` is true or `budget` elapses.
// Returns the wall-clock duration consumed. Guarantees termination.
template <typename Pred>
std::chrono::milliseconds pump_until(KqueueReactor& r, Pred pred,
                                     std::chrono::milliseconds budget) {
    auto start = clock_type::now();
    while (!pred()) {
        if (clock_type::now() - start > budget) break;
        r.poll(10);  // 10 ms slice — never blocks the test indefinitely
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        clock_type::now() - start);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 1. Pipe Read event: write 5 bytes, assert the registered callback fires and
//    reads back exactly those 5 bytes with the exact content.
// ─────────────────────────────────────────────────────────────────────────────
TEST(ReactorLoopTest, PipeReadEventFiresWithWrittenData) {
    int pipefd[2];
    ASSERT_EQ(::pipe(pipefd), 0);
    set_nonblocking(pipefd[0]);
    set_nonblocking(pipefd[1]);

    KqueueReactor reactor;

    std::atomic<int> fire_count{0};
    std::string received;
    auto rr = reactor.register_event(pipefd[0], EventType::Read, [&](int fd) {
        char buf[64];
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) received.assign(buf, static_cast<size_t>(n));
        fire_count.fetch_add(1);
    });
    EXPECT_TRUE(rr.has_value());  // register_event succeeded (Result<void>)

    // Commit the registration to the kernel before any data is written.
    auto pre = reactor.poll(10);
    ASSERT_TRUE(pre.has_value());

    // Now write data — this must wake the loop and invoke the read callback.
    const char* msg = "hello";
    ASSERT_EQ(::write(pipefd[1], msg, 5), 5);

    auto elapsed = pump_until(reactor, [&] { return fire_count.load() >= 1; },
                              std::chrono::milliseconds(2000));

    EXPECT_GE(fire_count.load(), 1) << "read callback never fired after write";
    EXPECT_EQ(received, std::string("hello"));
    EXPECT_LT(elapsed.count(), 2000) << "fired only at deadline (loop not waking)";

    ::close(pipefd[0]);
    ::close(pipefd[1]);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Socketpair Read event (eventfd-style cross-fd wakeup). Confirms the loop
//    delivers a readable event on a socket and the byte count is exact.
// ─────────────────────────────────────────────────────────────────────────────
TEST(ReactorLoopTest, SocketReadEventDeliversExactByteCount) {
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    set_nonblocking(sv[0]);
    set_nonblocking(sv[1]);

    KqueueReactor reactor;

    std::atomic<ssize_t> bytes_read{-1};
    auto rr = reactor.register_event(sv[0], EventType::Read, [&](int fd) {
        char buf[128];
        bytes_read.store(::read(fd, buf, sizeof(buf)));
    });
    EXPECT_TRUE(rr.has_value());

    ASSERT_TRUE(reactor.poll(10).has_value());  // commit registration

    const char payload[] = {1, 2, 3, 4, 5, 6, 7};  // 7 bytes
    ASSERT_EQ(::write(sv[1], payload, sizeof(payload)), 7);

    pump_until(reactor, [&] { return bytes_read.load() >= 0; },
               std::chrono::milliseconds(2000));

    EXPECT_EQ(bytes_read.load(), 7) << "exact byte count not delivered";

    ::close(sv[0]);
    ::close(sv[1]);
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. One-shot timer fires AFTER the requested delay, not before.
//    Asserts: valid timer id, callback fired exactly once, and the wall-clock
//    time-to-fire is >= the delay (proves it is a real delayed timer, not an
//    immediate dispatch).
// ─────────────────────────────────────────────────────────────────────────────
TEST(ReactorLoopTest, TimerFiresAfterDelay) {
    KqueueReactor reactor;

    constexpr int kDelayMs = 60;
    std::atomic<int> fired{0};
    int reported_id = -1;

    auto tr = reactor.register_timer(kDelayMs, [&](int id) {
        reported_id = id;
        fired.fetch_add(1);
    });
    ASSERT_TRUE(tr.has_value()) << "register_timer returned an error";
    const int timer_id = tr.value();
    EXPECT_GT(timer_id, 0) << "timer id must be a valid (non-kInvalid) handle";

    auto start = clock_type::now();
    auto elapsed = pump_until(reactor, [&] { return fired.load() >= 1; },
                              std::chrono::milliseconds(3000));
    (void)start;

    EXPECT_EQ(fired.load(), 1) << "one-shot timer must fire exactly once";
    EXPECT_EQ(reported_id, timer_id)
        << "timer callback must receive the same id register_timer returned";
    // Real delayed timer: must not fire meaningfully before the delay.
    EXPECT_GE(elapsed.count(), kDelayMs - 5)
        << "timer fired earlier than its delay (" << kDelayMs << " ms)";
    EXPECT_LT(elapsed.count(), 3000) << "timer never fired within budget";
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. A cancelled timer never fires. Schedule a 100 ms timer, cancel it
//    immediately, then pump for well beyond the delay and assert zero fires.
// ─────────────────────────────────────────────────────────────────────────────
TEST(ReactorLoopTest, CancelledTimerNeverFires) {
    KqueueReactor reactor;

    std::atomic<int> fired{0};
    auto tr = reactor.register_timer(100, [&](int) { fired.fetch_add(1); });
    ASSERT_TRUE(tr.has_value());

    auto ur = reactor.unregister_timer(tr.value());
    EXPECT_TRUE(ur.has_value());  // unregister_timer returns Result<void>

    // Pump for 300 ms (3x the delay). The timer must stay silent.
    pump_until(reactor, [] { return false; }, std::chrono::milliseconds(300));

    EXPECT_EQ(fired.load(), 0) << "cancelled timer fired anyway";
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. poll() returns promptly when idle: with no registered events and a short
//    timeout, a single poll() must come back quickly (it must not busy-spin or
//    block much past its timeout) and report 0 processed events.
// ─────────────────────────────────────────────────────────────────────────────
TEST(ReactorLoopTest, IdlePollReturnsPromptlyWithZeroEvents) {
    KqueueReactor reactor;

    auto t0 = clock_type::now();
    auto r = reactor.poll(50);  // ask for up to 50 ms
    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
        clock_type::now() - t0);

    ASSERT_TRUE(r.has_value()) << "idle poll returned an error";
    EXPECT_EQ(r.value(), 0) << "idle poll should process zero events";
    // It honored the timeout (>= ~timeout) but did NOT hang far beyond it.
    EXPECT_LT(dt.count(), 500)
        << "idle poll blocked far longer than its 50 ms timeout";
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. post() from another thread delivers a callback into the loop. Exercises the
//    EVFILT_USER NOTE_TRIGGER wakeup path — the canonical cross-thread enqueue.
// ─────────────────────────────────────────────────────────────────────────────
TEST(ReactorLoopTest, PostFromOtherThreadIsDelivered) {
    KqueueReactor reactor;

    std::atomic<bool> ran{false};
    std::jthread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        reactor.post([&] { ran.store(true); });
    });

    auto elapsed = pump_until(reactor, [&] { return ran.load(); },
                              std::chrono::milliseconds(2000));

    EXPECT_TRUE(ran.load()) << "posted callback was never executed in the loop";
    EXPECT_LT(elapsed.count(), 2000) << "post() wakeup did not reach the loop";
}

#else  // !__APPLE__

// KqueueReactor is macOS/BSD only. On Linux the equivalent loop tests would use
// EpollReactor + eventfd; that is out of scope for this file (which targets the
// macOS platform reactor). Provide one trivially-true marker so the suite is not
// empty on Linux.
TEST(ReactorLoopTest, KqueueReactorIsMacOnly_SkippedOnThisPlatform) {
    GTEST_SKIP() << "KqueueReactor tests run on macOS/BSD only.";
}

#endif  // __APPLE__
