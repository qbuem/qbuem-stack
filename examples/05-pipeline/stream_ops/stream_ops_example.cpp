/**
 * @file examples/stream_ops_example.cpp
 * @brief Stream operator examples — throttle, debounce, tumbling_window, map, filter
 */
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/async_channel.hpp>
#include <qbuem/pipeline/stream.hpp>
#include <qbuem/pipeline/stream_ops.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>
#include <qbuem/compat/print.hpp>

using namespace qbuem;
using namespace std::chrono_literals;
using std::println;

// Producer coroutines are FREE FUNCTIONS that take the channel BY VALUE, so the
// coroutine frame owns it. Spawning an immediately-invoked lambda
// (`spawn([ch]() -> Task<...> {...}())`) is undefined behaviour: the temporary
// closure is destroyed at the end of the spawn() statement while the worker
// thread still references it (use-after-scope / use-after-free).
static Task<Result<void>> feed_ints(std::shared_ptr<AsyncChannel<int>> ch,
                                    int from, int to) {
    for (int i = from; i <= to; ++i)
        ch->try_send(i);
    ch->close();
    co_return {};
}

static Task<Result<void>> feed_strings(std::shared_ptr<AsyncChannel<std::string>> ch,
                                       int count) {
    for (int i = 0; i < count; ++i)
        ch->try_send("event-" + std::to_string(i));
    ch->close();
    co_return {};
}

// ─── Basic Stream map / filter ───────────────────────────────────────────────

Task<Result<void>> stream_map_filter(Dispatcher& dispatcher) {
    auto ch = std::make_shared<AsyncChannel<int>>(64);

    // Source coroutine: publish 1..10
    dispatcher.spawn(feed_ints(ch, 1, 10));

    Stream<int> src(ch);

    // map: ×2
    auto doubled = src | stream_map([](int x) -> Task<Result<int>> {
        co_return x * 2;
    });

    // filter: even values only
    auto evens = doubled | stream_filter([](const int& x) {
        return x % 4 == 0; // divisible by 4 in doubled stream (even in original)
    });

    std::vector<int> results;
    while (true) {
        auto item = co_await evens.next();
        if (!item) break;
        results.push_back(*item);
        println("[stream] map×2+filter: {}", *item);
    }

    println("[stream] map+filter: {} items", results.size());
    co_return {};
}

// ─── stream_throttle ──────────────────────────────────────────────────────────

Task<Result<void>> stream_throttle_example(Dispatcher& dispatcher) {
    auto ch = std::make_shared<AsyncChannel<std::string>>(256);

    // Publish 20 items rapidly
    dispatcher.spawn(feed_strings(ch, 20));

    Stream<std::string> src(ch);

    // Throttle: max 5 per 100ms
    auto throttled = src | stream_throttle<std::string>(5, 100);

    size_t count = 0;
    while (true) {
        auto item = co_await throttled.next();
        if (!item) break;
        ++count;
    }
    println("[stream] throttle: {} events passed through", count);
    co_return {};
}

// ─── stream_debounce ──────────────────────────────────────────────────────────

Task<Result<void>> stream_debounce_example(Dispatcher& dispatcher) {
    auto ch = std::make_shared<AsyncChannel<int>>(64);

    // Publish 5 items rapidly then silence
    dispatcher.spawn(feed_ints(ch, 0, 4));

    Stream<int> src(ch);
    // Emit the last item after 30ms of silence
    auto debounced = src | stream_debounce<int>(30);

    size_t count = 0;
    while (true) {
        auto item = co_await debounced.next();
        if (!item) break;
        ++count;
        println("[stream] debounce: value={}", *item);
    }
    println("[stream] debounce: {} event(s) emitted", count);
    co_return {};
}

// ─── stream_tumbling_window ───────────────────────────────────────────────────

Task<Result<void>> stream_window_example(Dispatcher& dispatcher) {
    auto ch = std::make_shared<AsyncChannel<int>>(128);

    // Publish 30 items
    dispatcher.spawn(feed_ints(ch, 0, 29));

    Stream<int> src(ch);
    // 50ms tumbling window
    auto windowed = src | stream_tumbling_window<int>(50);

    size_t windows = 0;
    while (true) {
        auto window = co_await windowed.next();
        if (!window) break;
        ++windows;
        println("[stream] window {}: {} items", windows, window->size());
    }
    println("[stream] tumbling_window: {} windows", windows);
    co_return {};
}

// ─── main ─────────────────────────────────────────────────────────────────────

// Free-function orchestrator (frame owns its reference args) — see note above on
// why an immediately-invoked coroutine-lambda would be use-after-scope.
static Task<Result<void>> run_all(Dispatcher& dispatcher, std::atomic<int>& done) {
    co_await stream_map_filter(dispatcher);
    co_await stream_throttle_example(dispatcher);
    co_await stream_debounce_example(dispatcher);
    co_await stream_window_example(dispatcher);
    done.store(1);
    co_return {};
}

int main() {
    println("=== Stream Operators ===");

    Dispatcher dispatcher(2);
    std::jthread run_th([&] { dispatcher.run(); });

    std::atomic<int> done{0};

    dispatcher.spawn(run_all(dispatcher, done));

    auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!done.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);

    dispatcher.stop();
    run_th.join();
    return 0;
}
