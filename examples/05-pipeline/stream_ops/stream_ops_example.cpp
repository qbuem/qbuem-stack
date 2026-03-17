/**
 * @file examples/stream_ops_example.cpp
 * @brief Stream 연산자 예시 — throttle, debounce, tumbling_window, map, filter
 */
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/async_channel.hpp>
#include <qbuem/pipeline/stream.hpp>
#include <qbuem/pipeline/stream_ops.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;

// ─── 기본 Stream map / filter ────────────────────────────────────────────────

Task<Result<void>> stream_map_filter(Dispatcher& dispatcher) {
    auto ch = std::make_shared<AsyncChannel<int>>(64);

    // 소스 코루틴: 1~10 발행
    dispatcher.spawn([ch]() -> Task<Result<void>> {
        for (int i = 1; i <= 10; ++i)
            ch->try_send(i);
        ch->close();
        co_return {};
    }());

    Stream<int> src(ch);

    // map: ×2
    auto doubled = src | stream_map([](int x) -> Task<Result<int>> {
        co_return x * 2;
    });

    // filter: 짝수만
    auto evens = doubled | stream_filter([](const int& x) {
        return x % 4 == 0; // 원래 값 기준 짝수 (doubled 기준으론 4의 배수)
    });

    std::vector<int> results;
    while (true) {
        auto item = co_await evens.next();
        if (!item) break;
        results.push_back(*item);
        std::cout << "[stream] map×2+filter: " << *item << "\n";
    }

    std::cout << "[stream] map+filter: " << results.size() << " items\n";
    co_return {};
}

// ─── stream_throttle ─────────────────────────────────────────────────────────

Task<Result<void>> stream_throttle_example(Dispatcher& dispatcher) {
    auto ch = std::make_shared<AsyncChannel<std::string>>(256);

    // 빠르게 100개 발행
    dispatcher.spawn([ch]() -> Task<Result<void>> {
        for (int i = 0; i < 20; ++i)
            ch->try_send("event-" + std::to_string(i));
        ch->close();
        co_return {};
    }());

    Stream<std::string> src(ch);

    // 최대 5개/100ms 스로틀
    auto throttled = src | stream_throttle<std::string>(5, 100);

    size_t count = 0;
    while (true) {
        auto item = co_await throttled.next();
        if (!item) break;
        ++count;
    }
    std::cout << "[stream] throttle: " << count << " events passed through\n";
    co_return {};
}

// ─── stream_debounce ─────────────────────────────────────────────────────────

Task<Result<void>> stream_debounce_example(Dispatcher& dispatcher) {
    auto ch = std::make_shared<AsyncChannel<int>>(64);

    // 빠르게 5개 발행 후 조용함
    dispatcher.spawn([ch]() -> Task<Result<void>> {
        for (int i = 0; i < 5; ++i)
            ch->try_send(i);
        ch->close();
        co_return {};
    }());

    Stream<int> src(ch);
    // 30ms 조용한 구간 후 마지막 아이템 방출
    auto debounced = src | stream_debounce<int>(30);

    size_t count = 0;
    while (true) {
        auto item = co_await debounced.next();
        if (!item) break;
        ++count;
        std::cout << "[stream] debounce: value=" << *item << "\n";
    }
    std::cout << "[stream] debounce: " << count << " event(s) emitted\n";
    co_return {};
}

// ─── stream_tumbling_window ──────────────────────────────────────────────────

Task<Result<void>> stream_window_example(Dispatcher& dispatcher) {
    auto ch = std::make_shared<AsyncChannel<int>>(128);

    // 100개 발행
    dispatcher.spawn([ch]() -> Task<Result<void>> {
        for (int i = 0; i < 30; ++i)
            ch->try_send(i);
        ch->close();
        co_return {};
    }());

    Stream<int> src(ch);
    // 50ms 텀블링 윈도우
    auto windowed = src | stream_tumbling_window<int>(50);

    size_t windows = 0;
    while (true) {
        auto window = co_await windowed.next();
        if (!window) break;
        ++windows;
        std::cout << "[stream] window " << windows
                  << ": " << window->size() << " items\n";
    }
    std::cout << "[stream] tumbling_window: " << windows << " windows\n";
    co_return {};
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== Stream Operators ===\n";

    Dispatcher dispatcher(2);
    std::jthread run_th([&] { dispatcher.run(); });

    std::atomic<int> done{0};

    dispatcher.spawn([&]() -> Task<Result<void>> {
        co_await stream_map_filter(dispatcher);
        co_await stream_throttle_example(dispatcher);
        co_await stream_debounce_example(dispatcher);
        co_await stream_window_example(dispatcher);
        done.store(1);
        co_return {};
    }());

    auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!done.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);

    dispatcher.stop();
    run_th.join();
    return 0;
}
