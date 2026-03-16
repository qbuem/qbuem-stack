/**
 * @file windowed_action_example.cpp
 * @brief 이벤트 시간 기반 윈도우 처리 예제.
 *
 * ## 커버리지
 * - TumblingWindow             — 고정 크기 비중첩 윈도우
 * - SlidingWindow              — 슬라이딩 윈도우 (step < size)
 * - SessionWindow              — 세션 기반 윈도우 (갭 타임아웃)
 * - WindowedAction<T,Key,Acc,Out> — 키 기반 시간 윈도우 집계 액션
 * - Watermark                  — 비순서 이벤트 처리 진행 상태
 * - EventTime                  — Context 슬롯 (이벤트 발생 시각)
 * - WindowedAction::try_push() — 아이템 투입
 * - WindowedAction::start()    — 워커 + 틱커 시작
 * - WindowedAction::stop()     — 즉시 정지
 * - WindowedAction::output()   — 출력 채널
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/async_channel.hpp>
#include <qbuem/pipeline/context.hpp>
#include <qbuem/pipeline/windowed_action.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;
using Clock = std::chrono::system_clock;

// ─── 도메인 타입 ──────────────────────────────────────────────────────────────

struct ClickEvent {
    std::string user_id;
    std::string page;
    int         value{1};
};

struct WindowResult {
    std::string user_id;
    int         count{0};
    Clock::time_point window_start;
};

// ─────────────────────────────────────────────────────────────────────────────
// §1  TumblingWindow — 1초 단위 클릭 집계
// ─────────────────────────────────────────────────────────────────────────────

static void demo_tumbling() {
    std::printf("── §1  TumblingWindow (1초 윈도우, 클릭 집계) ──\n");

    WindowedAction<ClickEvent, std::string, int, WindowResult> wa{
        WindowedAction<ClickEvent, std::string, int, WindowResult>::Config{
            .type    = WindowType::Tumbling,
            .size    = 200ms,          // 200ms 윈도우 (데모용)
            .key_fn  = [](const ClickEvent& e) { return e.user_id; },
            .acc_fn  = [](int& acc, const ClickEvent& e) { acc += e.value; },
            .emit_fn = [](std::string uid, int cnt, Clock::time_point ws) {
                return WindowResult{std::move(uid), cnt, ws};
            },
            .init_acc    = 0,
            .channel_cap = 64,
        }};

    auto out_ch = std::make_shared<AsyncChannel<ContextualItem<WindowResult>>>(64);

    Dispatcher disp(1);
    std::thread t([&] { disp.run(); });

    wa.start(disp, out_ch);

    // 현재 시각 기준으로 이벤트 투입
    auto now = Clock::now();

    auto push_event = [&](std::string uid, int val, Clock::time_point ts) {
        Context ctx = Context{}.put(EventTime{ts});
        wa.try_push(ClickEvent{uid, "/home", val}, ctx);
    };

    // 첫 번째 윈도우: user_a 3번, user_b 2번
    push_event("user_a", 1, now);
    push_event("user_b", 1, now + 10ms);
    push_event("user_a", 1, now + 20ms);
    push_event("user_a", 1, now + 50ms);
    push_event("user_b", 1, now + 80ms);

    // 두 번째 윈도우: user_a 1번
    push_event("user_a", 1, now + 210ms);

    // 결과 수집 (최대 3개: user_a(1st), user_b(1st), user_a(2nd))
    std::vector<WindowResult> results;
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (results.size() < 3 && std::chrono::steady_clock::now() < deadline) {
        auto item = out_ch->try_recv();
        if (item) results.push_back(std::move(item->value));
        else std::this_thread::sleep_for(10ms);
    }

    wa.stop();
    disp.stop();
    t.join();

    for (auto& r : results) {
        std::printf("  [TumblingResult] user=%s count=%d\n",
                    r.user_id.c_str(), r.count);
    }
    std::printf("\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  SlidingWindow — 300ms 윈도우, 100ms 슬라이딩
// ─────────────────────────────────────────────────────────────────────────────

static void demo_sliding() {
    std::printf("── §2  SlidingWindow (300ms 윈도우, 100ms 스텝) ──\n");

    WindowedAction<ClickEvent, std::string, int, WindowResult> wa{
        WindowedAction<ClickEvent, std::string, int, WindowResult>::Config{
            .type    = WindowType::Sliding,
            .size    = 300ms,
            .step    = 100ms,
            .key_fn  = [](const ClickEvent& e) { return e.user_id; },
            .acc_fn  = [](int& acc, const ClickEvent& e) { acc += e.value; },
            .emit_fn = [](std::string uid, int cnt, Clock::time_point ws) {
                return WindowResult{std::move(uid), cnt, ws};
            },
            .init_acc    = 0,
            .channel_cap = 128,
        }};

    auto out_ch = std::make_shared<AsyncChannel<ContextualItem<WindowResult>>>(128);

    Dispatcher disp(1);
    std::thread t([&] { disp.run(); });
    wa.start(disp, out_ch);

    auto now = Clock::now();
    Context ctx = Context{}.put(EventTime{now + 50ms});
    wa.try_push(ClickEvent{"alice", "/", 1}, ctx);

    // 잠시 대기 후 결과 수집
    std::this_thread::sleep_for(600ms);

    std::vector<WindowResult> results;
    while (true) {
        auto item = out_ch->try_recv();
        if (!item) break;
        results.push_back(std::move(item->value));
    }

    wa.stop();
    disp.stop();
    t.join();

    std::printf("  SlidingWindow 결과 수: %zu (동일 이벤트가 여러 윈도우에 포함)\n",
                results.size());
    std::printf("\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// §3  SessionWindow — 갭 200ms 세션
// ─────────────────────────────────────────────────────────────────────────────

static void demo_session() {
    std::printf("── §3  SessionWindow (gap=200ms) ──\n");

    WindowedAction<ClickEvent, std::string, int, WindowResult> wa{
        WindowedAction<ClickEvent, std::string, int, WindowResult>::Config{
            .type    = WindowType::Session,
            .gap     = 200ms,
            .key_fn  = [](const ClickEvent& e) { return e.user_id; },
            .acc_fn  = [](int& acc, const ClickEvent& e) { acc += e.value; },
            .emit_fn = [](std::string uid, int cnt, Clock::time_point ws) {
                return WindowResult{std::move(uid), cnt, ws};
            },
            .init_acc    = 0,
            .channel_cap = 64,
        }};

    auto out_ch = std::make_shared<AsyncChannel<ContextualItem<WindowResult>>>(64);

    Dispatcher disp(1);
    std::thread t([&] { disp.run(); });
    wa.start(disp, out_ch);

    auto now = Clock::now();

    auto push = [&](std::string uid, Clock::time_point ts) {
        Context ctx = Context{}.put(EventTime{ts});
        wa.try_push(ClickEvent{std::move(uid), "/shop", 1}, ctx);
    };

    // 세션 1: bob — 연속 클릭 3회 (갭 없음)
    push("bob", now);
    push("bob", now + 50ms);
    push("bob", now + 100ms);

    // 세션 2: bob — 갭 300ms 후 새 세션
    push("bob", now + 400ms);

    std::this_thread::sleep_for(800ms);

    std::vector<WindowResult> results;
    while (true) {
        auto item = out_ch->try_recv();
        if (!item) break;
        results.push_back(std::move(item->value));
    }

    wa.stop();
    disp.stop();
    t.join();

    std::printf("  SessionWindow 결과 수: %zu\n", results.size());
    for (auto& r : results)
        std::printf("  [Session] user=%s count=%d\n", r.user_id.c_str(), r.count);
    std::printf("\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// §4  TumblingWindow 유틸리티 직접 사용
// ─────────────────────────────────────────────────────────────────────────────

static void demo_window_utils() {
    std::printf("── §4  윈도우 유틸리티 직접 사용 ──\n");

    // TumblingWindow: 특정 시각이 속한 윈도우 계산
    TumblingWindow tw{1000ms};
    auto t0 = Clock::time_point(std::chrono::seconds(5));
    auto wd = tw.window_for(t0);
    auto dur = std::chrono::duration_cast<std::chrono::seconds>(
        wd.end - wd.start).count();
    std::printf("  TumblingWindow(1s): window 크기 = %lds\n", dur);

    // SlidingWindow: 하나의 이벤트가 여러 윈도우에 속함
    SlidingWindow sw{500ms, 200ms};
    auto windows = sw.windows_for(t0);
    std::printf("  SlidingWindow(500ms/200ms): 이벤트가 %zu개 윈도우에 포함\n",
                windows.size());

    // SessionWindow: tick_interval = gap/2
    SessionWindow sess{400ms};
    std::printf("  SessionWindow(gap=400ms): tick_interval = %ldms\n",
                sess.tick_interval().count());

    // Watermark 생성
    Watermark wm{Clock::now()};
    std::printf("  Watermark 생성 완료\n\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::printf("=== qbuem WindowedAction 예제 ===\n\n");

    demo_window_utils();
    demo_tumbling();
    demo_sliding();
    demo_session();

    std::printf("=== 완료 ===\n");
    return 0;
}
