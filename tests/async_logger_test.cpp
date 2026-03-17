/**
 * @file tests/async_logger_test.cpp
 * @brief Unit tests for AsyncLogger (lock-free SPSC ring, start/log/stop/make_callback).
 */

#include <gtest/gtest.h>
#include <qbuem/core/async_logger.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace qbuem;
using namespace std::chrono_literals;

// ─── Basic lifecycle ──────────────────────────────────────────────────────────

TEST(AsyncLoggerTest, StartAndStop) {
    // Should not throw or deadlock
    AsyncLogger logger(64, stderr, LogFormat::Text);
    logger.start();
    logger.stop();
}

TEST(AsyncLoggerTest, StartAndStopJson) {
    AsyncLogger logger(64, stderr, LogFormat::Json);
    logger.start();
    logger.stop();
}

// ─── log() ────────────────────────────────────────────────────────────────────

TEST(AsyncLoggerTest, LogDoesNotBlockOrCrash) {
    AsyncLogger logger(256, stderr, LogFormat::Text);
    logger.start();

    // Log several entries — should all succeed without blocking
    for (int i = 0; i < 10; ++i) {
        logger.log("GET", "/api/test", 200, 123L);
    }

    logger.stop();  // Flushes remaining entries
}

TEST(AsyncLoggerTest, LogVariousMethods) {
    AsyncLogger logger(256, stderr, LogFormat::Text);
    logger.start();

    logger.log("GET",    "/api/users",  200, 100L);
    logger.log("POST",   "/api/orders", 201, 500L);
    logger.log("DELETE", "/api/items",  204, 50L);
    logger.log("PUT",    "/api/config", 400, 200L);

    logger.stop();
}

TEST(AsyncLoggerTest, LogWithLongPath) {
    AsyncLogger logger(256, stderr, LogFormat::Text);
    logger.start();

    // Path longer than LogEntry::kMaxPath (256) — should be truncated, not crash
    std::string long_path(300, '/');
    logger.log("GET", long_path, 200, 1L);

    logger.stop();
}

// ─── make_callback() ─────────────────────────────────────────────────────────

TEST(AsyncLoggerTest, MakeCallbackReturnsCallable) {
    AsyncLogger logger(64, stderr, LogFormat::Text);
    logger.start();

    auto cb = logger.make_callback();
    EXPECT_TRUE(static_cast<bool>(cb));

    // Invoke callback — should log without crashing
    cb("GET", "/healthz", 200, 10L);

    logger.stop();
}

TEST(AsyncLoggerTest, CallbackCanBeUsedAfterMake) {
    AsyncLogger logger(128, stderr, LogFormat::Text);
    logger.start();

    auto cb = logger.make_callback();
    for (int i = 0; i < 5; ++i) {
        cb("POST", "/api/v1/data", 201, static_cast<long>(i * 100));
    }

    logger.stop();
}

// ─── Concurrent log from multiple threads ────────────────────────────────────

TEST(AsyncLoggerTest, ConcurrentLogFromMultipleThreads) {
    // AsyncLogger is SPSC by design, but let's verify it handles multi-producer
    // gracefully (entries may be dropped, but no crash/UB).
    AsyncLogger logger(1024, stderr, LogFormat::Text);
    logger.start();

    std::vector<std::jthread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&logger, t] {
            for (int i = 0; i < 20; ++i)
                logger.log("GET", "/concurrent", 200, static_cast<long>(t * 100 + i));
        });
    }
    for (auto& th : threads) th.join();

    logger.stop();
}

// ─── Ring overflow (try to overflow the buffer) ───────────────────────────────

TEST(AsyncLoggerTest, RingOverflowDropsWithoutCrash) {
    // Ring of only 4 entries — overflow should drop silently
    AsyncLogger logger(4, stderr, LogFormat::Text);
    logger.start();

    for (int i = 0; i < 32; ++i)
        logger.log("GET", "/overflow", 200, static_cast<long>(i));

    logger.stop();  // Flushes what was captured
}
