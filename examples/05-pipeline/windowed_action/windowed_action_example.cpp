/**
 * @file windowed_action_example.cpp
 * @brief Event-time based windowed processing example.
 *
 * ## Coverage
 * - TumblingWindow             — fixed-size non-overlapping window
 * - SlidingWindow              — sliding window (step < size)
 * - SessionWindow              — session-based window (gap timeout)
 * - WindowedAction<T,Key,Acc,Out> — key-based time-window aggregation action
 * - Watermark                  — out-of-order event processing progress state
 * - EventTime                  — Context slot (event occurrence timestamp)
 * - WindowedAction::try_push() — push item
 * - WindowedAction::start()    — start workers + ticker
 * - WindowedAction::stop()     — immediate stop
 * - WindowedAction::output()   — output channel
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/async_channel.hpp>
#include <qbuem/pipeline/context.hpp>
#include <qbuem/pipeline/windowed_action.hpp>

#include <chrono>
#include <string>
#include <thread>
#include <vector>
#include <qbuem/compat/print.hpp>

using namespace qbuem;
using namespace std::chrono_literals;
using std::println;
using Clock = std::chrono::system_clock;

// ─── Domain types ─────────────────────────────────────────────────────────────

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
// §1  TumblingWindow — 1-second click aggregation
// ─────────────────────────────────────────────────────────────────────────────

static void demo_tumbling() {
    println("── §1  TumblingWindow (1s window, click aggregation) ──");

    WindowedAction<ClickEvent, std::string, int, WindowResult> wa{
        WindowedAction<ClickEvent, std::string, int, WindowResult>::Config{
            .type    = WindowType::Tumbling,
            .size    = 200ms,          // 200ms window (for demo)
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
    std::jthread t([&] { disp.run(); });

    wa.start(disp, out_ch);

    // Push events relative to current time
    auto now = Clock::now();

    auto push_event = [&](std::string uid, int val, Clock::time_point ts) {
        Context ctx = Context{}.put(EventTime{ts});
        wa.try_push(ClickEvent{uid, "/home", val}, ctx);
    };

    // First window: user_a 3 clicks, user_b 2 clicks
    push_event("user_a", 1, now);
    push_event("user_b", 1, now + 10ms);
    push_event("user_a", 1, now + 20ms);
    push_event("user_a", 1, now + 50ms);
    push_event("user_b", 1, now + 80ms);

    // Second window: user_a 1 click
    push_event("user_a", 1, now + 210ms);

    // Collect results (up to 3: user_a(1st), user_b(1st), user_a(2nd))
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
        println("  [TumblingResult] user={} count={}", r.user_id, r.count);
    }
    println("");
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  SlidingWindow — 300ms window, 100ms step
// ─────────────────────────────────────────────────────────────────────────────

static void demo_sliding() {
    println("── §2  SlidingWindow (300ms window, 100ms step) ──");

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
    std::jthread t([&] { disp.run(); });
    wa.start(disp, out_ch);

    auto now = Clock::now();
    Context ctx = Context{}.put(EventTime{now + 50ms});
    wa.try_push(ClickEvent{"alice", "/", 1}, ctx);

    // Brief wait then collect results
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

    println("  SlidingWindow result count: {} (same event appears in multiple windows)",
                results.size());
    println("");
}

// ─────────────────────────────────────────────────────────────────────────────
// §3  SessionWindow — 200ms gap session
// ─────────────────────────────────────────────────────────────────────────────

static void demo_session() {
    println("── §3  SessionWindow (gap=200ms) ──");

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
    std::jthread t([&] { disp.run(); });
    wa.start(disp, out_ch);

    auto now = Clock::now();

    auto push = [&](std::string uid, Clock::time_point ts) {
        Context ctx = Context{}.put(EventTime{ts});
        wa.try_push(ClickEvent{std::move(uid), "/shop", 1}, ctx);
    };

    // Session 1: bob — 3 consecutive clicks (no gap)
    push("bob", now);
    push("bob", now + 50ms);
    push("bob", now + 100ms);

    // Session 2: bob — new session after 300ms gap
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

    println("  SessionWindow result count: {}", results.size());
    for (auto& r : results)
        println("  [Session] user={} count={}", r.user_id, r.count);
    println("");
}

// ─────────────────────────────────────────────────────────────────────────────
// §4  Direct window utility usage
// ─────────────────────────────────────────────────────────────────────────────

static void demo_window_utils() {
    println("── §4  Window Utility Direct Usage ──");

    // TumblingWindow: compute the window containing a given timestamp
    TumblingWindow tw{1000ms};
    auto t0 = Clock::time_point(std::chrono::seconds(5));
    auto wd = tw.window_for(t0);
    auto dur = std::chrono::duration_cast<std::chrono::seconds>(
        wd.end - wd.start).count();
    println("  TumblingWindow(1s): window size = {}s", dur);

    // SlidingWindow: one event can belong to multiple windows
    SlidingWindow sw{500ms, 200ms};
    auto windows = sw.windows_for(t0);
    println("  SlidingWindow(500ms/200ms): event belongs to {} windows",
                windows.size());

    // SessionWindow: tick_interval = gap/2
    SessionWindow sess{400ms};
    println("  SessionWindow(gap=400ms): tick_interval = {}ms",
                sess.tick_interval().count());

    // Create a Watermark
    Watermark wm{Clock::now()};
    println("  Watermark created\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    println("=== qbuem WindowedAction Example ===\n");

    demo_window_utils();
    demo_tumbling();
    demo_sliding();
    demo_session();

    println("=== Done ===");
    return 0;
}
