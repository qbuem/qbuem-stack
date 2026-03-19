/**
 * @file zero_copy_arena_channel_example.cpp
 * @brief Zero-copy I/O + ArenaChannel + AsyncLogger example.
 *
 * ## Coverage — io/zero_copy.hpp
 * - zero_copy::sendfile()      — sendfile(2)-based file→socket zero-copy
 * - zero_copy::splice()        — splice(2)-based pipe-based zero-copy
 *
 * ## Coverage — pipeline/arena_channel.hpp
 * - ArenaChannel<T>            — reactor-local zero-alloc channel
 * - ArenaChannel::push()       — O(1) non-blocking send (FixedPoolResource-based)
 * - ArenaChannel::pop()        — O(1) non-blocking receive
 * - ArenaChannel::size()       — current channel size
 * - ArenaChannel::capacity()   — channel capacity
 *
 * ## Coverage — core/async_logger.hpp
 * - AsyncLogger(capacity)      — create ring-buffer async logger
 * - AsyncLogger::start()       — start background flush thread
 * - AsyncLogger::log()         — hot-path log enqueue (O(1), non-blocking)
 * - AsyncLogger::stop()        — flush remaining entries then shutdown
 * - AsyncLogger::make_callback()— create App access-logger callback
 */

#include <qbuem/core/async_logger.hpp>
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/io/zero_copy.hpp>
#include <qbuem/pipeline/arena_channel.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <qbuem/compat/print.hpp>

using namespace qbuem;
using namespace std::chrono_literals;
using std::println;
using std::print;

// ─────────────────────────────────────────────────────────────────────────────
// §1  ArenaChannel — reactor-local zero-alloc channel
// ─────────────────────────────────────────────────────────────────────────────

static void demo_arena_channel() {
    println("── §1  ArenaChannel ──");

    // Create ArenaChannel with 128 slots (single heap allocation)
    ArenaChannel<int> chan(128);
    println("  capacity: {}", chan.capacity());

    // Producer: push
    for (int i = 0; i < 10; ++i) {
        bool ok = chan.push(i * 100);
        if (!ok) { println("  push failed (channel full)"); break; }
    }
    println("  size after 10 pushes: {}", chan.size());

    // Consumer: pop
    std::vector<int> results;
    while (auto v = chan.pop())
        results.push_back(*v);
    print("  pop results ({}):", results.size());
    for (int v : results) print(" {}", v);
    println("");

    // Capacity test: push returns false when full
    ArenaChannel<std::string> small_chan(4);
    for (int i = 0; i < 6; ++i) {
        bool ok = small_chan.push("item" + std::to_string(i));
        println("  small_chan push({}): {}", i, ok ? "ok" : "full");
    }
    println("");
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  Zero-copy I/O — sendfile / splice
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<bool> g_zero_copy_done{false};

static Task<void> demo_zero_copy_task() {
    println("── §2  Zero-copy I/O ──");

    // Create a temporary file
    char tmpfile[] = "/tmp/qbuem_zero_copy_XXXXXX";
    int in_fd = ::mkstemp(tmpfile);
    if (in_fd < 0) {
        println("  Failed to create temporary file");
        g_zero_copy_done.store(true);
        co_return;
    }

    // Write data to the file
    const char* content = "Hello, zero-copy sendfile world!\n"
                          "This data is transferred without user-space copy.\n";
    ssize_t written = ::write(in_fd, content, strlen(content));
    ::lseek(in_fd, 0, SEEK_SET);  // rewind to beginning

    println("  Temporary file created: {} ({} bytes)", tmpfile, written);

    // Create socket pair for sendfile test
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        println("  socketpair failed");
        ::close(in_fd);
        ::unlink(tmpfile);
        g_zero_copy_done.store(true);
        co_return;
    }

    // sendfile: file → socket (zero-copy)
    auto sent_r = co_await zero_copy::sendfile(
        sv[1],       // output: socket
        in_fd,       // input: file
        0,           // offset
        written      // bytes to transfer
    );

    if (sent_r) {
        println("  sendfile(): {} bytes transferred", *sent_r);

        // Read data on the receiving side
        std::array<char, 256> buf{};
        ssize_t n = ::recv(sv[0], buf.data(), buf.size(), MSG_DONTWAIT);
        if (n > 0) {
            println("  received: {} bytes (content match: {})",
                        n, (memcmp(buf.data(), content, n) == 0) ? "yes" : "no");
        }
    } else {
        println("  sendfile(): {}", sent_r.error().message());
    }

#ifdef __linux__
    // splice: pipe-based zero-copy (Linux only)
    int pipefd[2];
    if (::pipe(pipefd) == 0) {
        ::lseek(in_fd, 0, SEEK_SET);

        auto splice_r = co_await zero_copy::splice(
            in_fd,      // input fd
            pipefd[1],  // pipe write end
            written     // bytes to transfer
        );

        if (splice_r) {
            println("  splice(file→pipe): {} bytes", *splice_r);

            // splice from pipe to socket
            auto splice2_r = co_await zero_copy::splice(
                pipefd[0],  // pipe read end
                sv[1],      // socket
                *splice_r
            );
            if (splice2_r)
                println("  splice(pipe→socket): {} bytes", *splice2_r);
        } else {
            println("  splice(): {}", splice_r.error().message());
        }

        ::close(pipefd[0]);
        ::close(pipefd[1]);
    }
#else
    println("  splice(): Linux-only (not supported on this platform)");
#endif

    ::close(sv[0]);
    ::close(sv[1]);
    ::close(in_fd);
    ::unlink(tmpfile);
    println("");

    g_zero_copy_done.store(true);
    co_return;
}

// ─────────────────────────────────────────────────────────────────────────────
// §3  AsyncLogger — asynchronous access logger
// ─────────────────────────────────────────────────────────────────────────────

static void demo_async_logger() {
    println("── §3  AsyncLogger ──");

    // 4096-entry ring buffer logger
    AsyncLogger logger(4096);
    logger.start();  // start background flush thread

    println("  Enqueuing 100 log entries...");

    // O(1) non-blocking log writes on the hot path
    for (int i = 0; i < 100; ++i) {
        logger.log(
            "GET",                           // HTTP method
            "/api/users/" + std::to_string(i), // path
            200,                             // status code
            (i % 10) * 100 + 500             // response time (µs)
        );
    }

    // Error logs
    logger.log("POST", "/api/orders", 500, 25000);
    logger.log("DELETE", "/api/users/42", 403, 800);

    // Create App access-logger callback
    auto cb = logger.make_callback();
    cb("GET", "/health", 200, 50);  // log via callback

    println("  103 log entries written");

    logger.stop();  // flush remaining entries then stop thread
    println("  AsyncLogger stopped\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    println("=== qbuem Zero-copy + ArenaChannel + AsyncLogger Example ===\n");

    demo_arena_channel();
    demo_async_logger();

    // Zero-copy is coroutine-based
    Dispatcher disp(1);
    std::jthread t([&] { disp.run(); });

    disp.spawn([&]() -> Task<void> { co_await demo_zero_copy_task(); }());

    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!g_zero_copy_done.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);

    disp.stop();
    t.join();

    println("=== Done ===");
    return 0;
}
