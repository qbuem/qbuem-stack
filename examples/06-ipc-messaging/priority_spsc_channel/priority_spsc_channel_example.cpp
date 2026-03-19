/**
 * @file priority_spsc_channel_example.cpp
 * @brief PriorityChannel + SpscChannel example.
 *
 * ## Coverage
 * - Priority enum (High, Normal, Low)
 * - PriorityChannel<T>         — 3-level priority MPMC channel
 * - PriorityChannel::try_send()— priority-tagged send
 * - PriorityChannel::try_recv()— receive in High → Normal → Low order
 * - PriorityChannel::recv()    — async blocking receive
 * - PriorityChannel::close()   — close channel
 * - SpscChannel<T>             — SPSC lock-free ring buffer channel
 * - SpscChannel::try_send()    — non-blocking send
 * - SpscChannel::try_recv()    — non-blocking receive
 * - SpscChannel::send()        — async blocking send
 * - SpscChannel::recv()        — async blocking receive
 * - SpscChannel::capacity()    — channel capacity
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/priority_channel.hpp>
#include <qbuem/pipeline/spsc_channel.hpp>

#include <atomic>
#include <cassert>
#include <string>
#include <thread>
#include <vector>
#include <qbuem/compat/print.hpp>

using namespace qbuem;
using namespace std::chrono_literals;
using std::println;
using std::print;

// ─────────────────────────────────────────────────────────────────────────────
// §1  PriorityChannel — priority-based message processing
// ─────────────────────────────────────────────────────────────────────────────

struct Message {
    std::string content;
    int         id;
};

static void demo_priority_channel() {
    println("── §1  PriorityChannel ──");

    // Capacity 32 per priority level
    PriorityChannel<Message> chan(32);

    // Push messages with various priorities
    chan.try_send(Message{"payment processing",  1}, Priority::High);
    chan.try_send(Message{"log collection",       2}, Priority::Low);
    chan.try_send(Message{"order confirmation",   3}, Priority::Normal);
    chan.try_send(Message{"alert notification",   4}, Priority::High);
    chan.try_send(Message{"report generation",    5}, Priority::Low);
    chan.try_send(Message{"inventory check",      6}, Priority::Normal);
    chan.try_send(Message{"emergency rollback",   7}, Priority::High);

    println("  total channel size: {}", chan.size_approx());
    println("  receive order (High -> Normal -> Low):");

    std::vector<Message> received;
    while (auto item = chan.try_recv()) {
        received.push_back(*item);
        println("    [{}] id={}", item->content, item->id);
    }

    // Priority order verification: first 3 should be High (id 1, 4, 7)
    if (received.size() >= 3) {
        bool high_first = (received[0].id == 1 || received[0].id == 4 || received[0].id == 7);
        println("  High priority processed first: {}", high_first ? "yes" : "no");
    }

    println("");
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  PriorityChannel — async recv (coroutine)
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<int>  g_priority_received{0};

static Task<void> priority_consumer(
    std::shared_ptr<PriorityChannel<int>> chan, int expected) {
    for (int i = 0; i < expected; ++i) {
        auto val = co_await chan->recv();
        if (!val) break;
        g_priority_received.fetch_add(1, std::memory_order_relaxed);
    }
    co_return;
}

static Task<void> priority_producer(
    std::shared_ptr<PriorityChannel<int>> chan) {
    // Push numbers with various priorities
    for (int i = 1; i <= 5; ++i) {
        auto prio = (i % 3 == 0) ? Priority::High :
                    (i % 3 == 1) ? Priority::Normal : Priority::Low;
        co_await chan->send(i, prio);
    }
    chan->close();
    co_return;
}

static void demo_priority_async() {
    println("── §2  PriorityChannel async recv ──");

    auto chan = std::make_shared<PriorityChannel<int>>(16);

    auto consumer_fn = [chan]() -> Task<void> {
        co_await priority_consumer(chan, 5);
    };
    auto producer_fn = [chan]() -> Task<void> {
        co_await priority_producer(chan);
    };

    Dispatcher disp(2);
    std::jthread t([&] { disp.run(); });

    disp.spawn(consumer_fn());
    disp.spawn(producer_fn());

    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (g_priority_received.load() < 5 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);

    disp.stop();
    t.join();

    println("  items received: {} / 5\n", g_priority_received.load());
}

// ─────────────────────────────────────────────────────────────────────────────
// §3  SpscChannel — single producer/consumer lock-free channel
// ─────────────────────────────────────────────────────────────────────────────

static void demo_spsc_channel() {
    println("── §3  SpscChannel (try_send/try_recv) ──");

    SpscChannel<int> chan(16);  // capacity 16 (rounded up to power of two)

    println("  channel capacity: {}", chan.capacity());

    // Producer: try_send
    for (int i = 0; i < 8; ++i)
        chan.try_send(i * 10);

    println("  channel size: {}", chan.size_approx());

    // Consumer: try_recv
    std::vector<int> results;
    while (auto item = chan.try_recv()) {
        results.push_back(*item);
    }

    print("  received values:");
    for (int v : results) print(" {}", v);
    println("\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// §4  SpscChannel — async send/recv (coroutine, cross-thread)
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<int> g_spsc_sum{0};

static void demo_spsc_async() {
    println("── §4  SpscChannel async (cross-reactor) ──");

    auto chan = std::make_shared<SpscChannel<int>>(8);
    constexpr int N = 20;

    // Producer coroutine
    auto producer = [chan]() -> Task<void> {
        for (int i = 1; i <= N; ++i) {
            co_await chan->send(i);
        }
        chan->close();
        co_return;
    };

    // Consumer coroutine
    auto consumer = [chan]() -> Task<void> {
        for (;;) {
            auto v = co_await chan->recv();
            if (!v) break;
            g_spsc_sum.fetch_add(*v, std::memory_order_relaxed);
        }
        co_return;
    };

    Dispatcher disp(2);
    std::jthread t([&] { disp.run(); });

    disp.spawn(consumer());
    disp.spawn(producer());

    auto deadline = std::chrono::steady_clock::now() + 3s;
    // 1+2+...+20 = 210
    while (g_spsc_sum.load() < 210 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);

    disp.stop();
    t.join();

    println("  sum(1..{}) = {} (expected: 210)\n", N, g_spsc_sum.load());
}

// ─────────────────────────────────────────────────────────────────────────────
// §5  SpscChannel capacity round-up
// ─────────────────────────────────────────────────────────────────────────────

static void demo_spsc_capacity() {
    println("── §5  SpscChannel capacity auto round-up ──");

    // Requested capacity is rounded up to the next power of two
    SpscChannel<int> c7(7);    // → 8
    SpscChannel<int> c10(10);  // → 16
    SpscChannel<int> c100(100);// → 128

    println("  cap(7) = {}", c7.capacity());
    println("  cap(10) = {}", c10.capacity());
    println("  cap(100) = {}\n", c100.capacity());
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    println("=== qbuem PriorityChannel + SpscChannel Example ===\n");

    demo_priority_channel();
    demo_priority_async();
    demo_spsc_channel();
    demo_spsc_async();
    demo_spsc_capacity();

    println("=== Done ===");
    return 0;
}
