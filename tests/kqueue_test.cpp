#include <qbuem/core/kqueue_reactor.hpp>
#include <qbuem/common.hpp>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <csignal>

using namespace qbuem;

class KqueueReactorTest : public ::testing::Test {
protected:
    void SetUp() override {
        reactor = std::make_unique<KqueueReactor>();
    }

    void TearDown() override {
        reactor.reset();
    }

    std::unique_ptr<KqueueReactor> reactor;
};

// Test basic Read/Write with a socket pair
TEST_F(KqueueReactorTest, SocketPairReadWrite) {
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    bool read_called = false;
    bool write_called = false;

    // Register Read on fds[0]
    reactor->register_event(fds[0], EventType::Read, [&](int fd) {
        char buf[16];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) read_called = true;
    });

    // Register Write on fds[1]
    reactor->register_event(fds[1], EventType::Write, [&](int fd) {
        const char* msg = "hello";
        write(fd, msg, 5);
        write_called = true;
        reactor->unregister_event(fd, EventType::Write);
    });

    // Poll until both are called or timeout
    int iterations = 0;
    while (!(read_called && write_called) && iterations < 10) {
        reactor->poll(10);
        iterations++;
    }

    EXPECT_TRUE(read_called);
    EXPECT_TRUE(write_called);

    close(fds[0]);
    close(fds[1]);
}

// Test edge-triggered (EV_CLEAR) behavior
TEST_F(KqueueReactorTest, EdgeTriggeredBehavior) {
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    int read_count = 0;
    reactor->register_event(fds[0], EventType::Read, [&](int fd) {
        read_count++;
        // We DON'T read all data here, so in level-triggered it would fire again.
        // But with EV_CLEAR (Edge), it should not fire again until more data arrives.
    });

    // Write some data
    write(fds[1], "a", 1);
    
    // Poll twice
    reactor->poll(10);
    EXPECT_EQ(read_count, 1);
    
    reactor->poll(10);
    EXPECT_EQ(read_count, 1); // Should still be 1 because of EV_CLEAR

    // Write more data
    write(fds[1], "b", 1);
    reactor->poll(10);
    EXPECT_EQ(read_count, 2);

    close(fds[0]);
    close(fds[1]);
}

// Test Signal handling
TEST_F(KqueueReactorTest, SignalHandling) {
    bool signal_called = false;
    reactor->register_signal(SIGUSR1, [&](int sig) {
        if (sig == SIGUSR1) signal_called = true;
    });

    // Commit the registration to the kernel
    reactor->poll(0);

    // Ignore SIGUSR1 so it doesn't kill the process, letting kqueue handle it
    auto old_handler = signal(SIGUSR1, SIG_IGN);

    // Trigger signal
    kill(getpid(), SIGUSR1);

    // Poll a few times to ensure we catch it
    int iterations = 0;
    while (!signal_called && iterations < 20) {
        reactor->poll(50);
        iterations++;
    }
    EXPECT_TRUE(signal_called);

    reactor->unregister_signal(SIGUSR1);
    
    // Restore handler
    signal(SIGUSR1, old_handler);
}

// Test post() from another thread
TEST_F(KqueueReactorTest, PostFromThread) {
    std::atomic<bool> called{false};
    std::thread t([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        reactor->post([&]() {
            called = true;
        });
    });

    while (!called) {
        reactor->poll(10);
    }

    EXPECT_TRUE(called);
    t.join();
}
