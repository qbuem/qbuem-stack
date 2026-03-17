/**
 * @file priority_spsc_channel_example.cpp
 * @brief PriorityChannel + SpscChannel 예제.
 *
 * ## 커버리지
 * - Priority enum (High, Normal, Low)
 * - PriorityChannel<T>         — 3단계 우선순위 MPMC 채널
 * - PriorityChannel::try_send()— 우선순위 지정 송신
 * - PriorityChannel::try_recv()— High → Normal → Low 순서 수신
 * - PriorityChannel::recv()    — 비동기 블로킹 수신
 * - PriorityChannel::close()   — 채널 닫기
 * - SpscChannel<T>             — SPSC 락-프리 링 버퍼 채널
 * - SpscChannel::try_send()    — 논블로킹 송신
 * - SpscChannel::try_recv()    — 논블로킹 수신
 * - SpscChannel::send()        — 비동기 블로킹 송신
 * - SpscChannel::recv()        — 비동기 블로킹 수신
 * - SpscChannel::capacity()    — 채널 용량
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/priority_channel.hpp>
#include <qbuem/pipeline/spsc_channel.hpp>

#include <atomic>
#include <cassert>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// §1  PriorityChannel — 우선순위 기반 메시지 처리
// ─────────────────────────────────────────────────────────────────────────────

struct Message {
    std::string content;
    int         id;
};

static void demo_priority_channel() {
    std::printf("── §1  PriorityChannel ──\n");

    // 각 우선순위 레벨당 용량 32
    PriorityChannel<Message> chan(32);

    // 다양한 우선순위로 메시지 투입
    chan.try_send(Message{"결제 처리",    1}, Priority::High);
    chan.try_send(Message{"로그 수집",    2}, Priority::Low);
    chan.try_send(Message{"주문 확인",    3}, Priority::Normal);
    chan.try_send(Message{"경보 알림",    4}, Priority::High);
    chan.try_send(Message{"보고서 생성",  5}, Priority::Low);
    chan.try_send(Message{"재고 확인",    6}, Priority::Normal);
    chan.try_send(Message{"긴급 롤백",    7}, Priority::High);

    std::printf("  채널 크기 합계: %zu\n", chan.size_approx());
    std::printf("  수신 순서 (High → Normal → Low):\n");

    std::vector<Message> received;
    while (auto item = chan.try_recv()) {
        received.push_back(*item);
        std::printf("    [%s] id=%d\n", item->content.c_str(), item->id);
    }

    // 우선순위 순서 검증: 처음 3개는 High (id 1, 4, 7)
    if (received.size() >= 3) {
        bool high_first = (received[0].id == 1 || received[0].id == 4 || received[0].id == 7);
        std::printf("  High 우선순위 먼저 처리: %s\n", high_first ? "yes" : "no");
    }

    std::printf("\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  PriorityChannel — 비동기 recv (코루틴)
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
    // 다양한 우선순위로 숫자 투입
    for (int i = 1; i <= 5; ++i) {
        auto prio = (i % 3 == 0) ? Priority::High :
                    (i % 3 == 1) ? Priority::Normal : Priority::Low;
        co_await chan->send(i, prio);
    }
    chan->close();
    co_return;
}

static void demo_priority_async() {
    std::printf("── §2  PriorityChannel 비동기 recv ──\n");

    auto chan = std::make_shared<PriorityChannel<int>>(16);

    auto consumer_fn = [chan]() -> Task<void> {
        co_await priority_consumer(chan, 5);
    };
    auto producer_fn = [chan]() -> Task<void> {
        co_await priority_producer(chan);
    };

    Dispatcher disp(2);
    std::thread t([&] { disp.run(); });

    disp.spawn(consumer_fn());
    disp.spawn(producer_fn());

    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (g_priority_received.load() < 5 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);

    disp.stop();
    t.join();

    std::printf("  수신된 아이템: %d / 5\n\n",
                g_priority_received.load());
}

// ─────────────────────────────────────────────────────────────────────────────
// §3  SpscChannel — 단일 생산자/소비자 락-프리 채널
// ─────────────────────────────────────────────────────────────────────────────

static void demo_spsc_channel() {
    std::printf("── §3  SpscChannel (try_send/try_recv) ──\n");

    SpscChannel<int> chan(16);  // 16개 용량 (2의 거듭제곱으로 올림)

    std::printf("  채널 용량: %zu\n", chan.capacity());

    // 생산자: try_send
    for (int i = 0; i < 8; ++i)
        chan.try_send(i * 10);

    std::printf("  채널 크기: %zu\n", chan.size_approx());

    // 소비자: try_recv
    std::vector<int> results;
    while (auto item = chan.try_recv()) {
        results.push_back(*item);
    }

    std::printf("  수신한 값:");
    for (int v : results) std::printf(" %d", v);
    std::printf("\n\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// §4  SpscChannel — 비동기 send/recv (코루틴, 크로스-스레드)
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<int> g_spsc_sum{0};

static void demo_spsc_async() {
    std::printf("── §4  SpscChannel 비동기 (크로스-리액터) ──\n");

    auto chan = std::make_shared<SpscChannel<int>>(8);
    constexpr int N = 20;

    // 생산자 코루틴
    auto producer = [chan]() -> Task<void> {
        for (int i = 1; i <= N; ++i) {
            co_await chan->send(i);
        }
        chan->close();
        co_return;
    };

    // 소비자 코루틴
    auto consumer = [chan]() -> Task<void> {
        for (;;) {
            auto v = co_await chan->recv();
            if (!v) break;
            g_spsc_sum.fetch_add(*v, std::memory_order_relaxed);
        }
        co_return;
    };

    Dispatcher disp(2);
    std::thread t([&] { disp.run(); });

    disp.spawn(consumer());
    disp.spawn(producer());

    auto deadline = std::chrono::steady_clock::now() + 3s;
    // 1+2+...+20 = 210
    while (g_spsc_sum.load() < 210 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);

    disp.stop();
    t.join();

    std::printf("  sum(1..%d) = %d (기대: 210)\n\n", N, g_spsc_sum.load());
}

// ─────────────────────────────────────────────────────────────────────────────
// §5  SpscChannel vs AsyncChannel 비교
// ─────────────────────────────────────────────────────────────────────────────

static void demo_spsc_capacity() {
    std::printf("── §5  SpscChannel 용량 자동 올림 ──\n");

    // 요청 용량이 2의 거듭제곱으로 올림 처리됨
    SpscChannel<int> c7(7);    // → 8
    SpscChannel<int> c10(10);  // → 16
    SpscChannel<int> c100(100);// → 128

    std::printf("  cap(7) = %zu\n", c7.capacity());
    std::printf("  cap(10) = %zu\n", c10.capacity());
    std::printf("  cap(100) = %zu\n\n", c100.capacity());
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::printf("=== qbuem PriorityChannel + SpscChannel 예제 ===\n\n");

    demo_priority_channel();
    demo_priority_async();
    demo_spsc_channel();
    demo_spsc_async();
    demo_spsc_capacity();

    std::printf("=== 완료 ===\n");
    return 0;
}
