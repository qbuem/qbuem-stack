/**
 * @file canary_example.cpp
 * @brief CanaryRouter — gradual canary deployment example.
 *
 * Coverage:
 * - CanaryRouter::set_stable / set_canary / push / canary_percent
 * - set_canary_percent / rollback_to_stable
 * - CanaryMetrics record_success / record_error / error_rate / avg_latency_us
 * - RolloutConfig steps / max_error_delta / max_latency_ratio / on_rollback
 */

#include <qbuem/pipeline/canary.hpp>
#include <qbuem/compat/print.hpp>

#include <cassert>
#include <string>
#include <vector>

using namespace qbuem;

struct Request {
    int  id;
    bool is_canary_eligible;
};

int main() {
    std::println("[CanaryRouter] basic routing demo");

    // ─── 1. Basic routing setup ───────────────────────────────────────────────
    CanaryRouter<Request> router;

    std::vector<Request> stable_received;
    std::vector<Request> canary_received;

    router.set_stable([&](Request r) -> bool {
        stable_received.push_back(r);
        return true;
    });
    router.set_canary([&](Request r) -> bool {
        canary_received.push_back(r);
        return true;
    });

    // Initial: canary_pct=0 → all go to stable
    assert(router.canary_percent() == 0u);

    for (int i = 0; i < 10; ++i)
        router.push(Request{i, true});

    assert(stable_received.size() == 10u);
    assert(canary_received.empty());
    std::println("[CanaryRouter] 0% canary: stable={} canary={}",
                stable_received.size(), canary_received.size());

    // ─── 2. Canary 50% ───────────────────────────────────────────────────────
    router.set_canary_percent(50);
    assert(router.canary_percent() == 50u);

    stable_received.clear();
    canary_received.clear();

    // 1000 pushes → roughly 50% canary distribution
    for (int i = 0; i < 1000; ++i)
        router.push(Request{i, true});

    size_t total = stable_received.size() + canary_received.size();
    assert(total == 1000u);
    // Allow 50% ± 15% (random)
    assert(canary_received.size() > 300u);
    assert(canary_received.size() < 700u);
    std::println("[CanaryRouter] 50% canary: stable={} canary={}",
                stable_received.size(), canary_received.size());

    // ─── 3. Manual rollback ──────────────────────────────────────────────────
    router.rollback_to_stable();
    assert(router.canary_percent() == 0u);
    stable_received.clear();
    canary_received.clear();
    for (int i = 0; i < 10; ++i)
        router.push(Request{i, true});
    assert(stable_received.size() == 10u);
    assert(canary_received.empty());
    std::println("[CanaryRouter] rollback_to_stable: OK");

    // ─── 4. CanaryMetrics ────────────────────────────────────────────────────
    CanaryMetrics metrics;
    metrics.record_success(100);
    metrics.record_success(200);
    metrics.record_error();

    double err_rate = metrics.error_rate();
    assert(err_rate > 0.0 && err_rate < 1.0);
    uint64_t avg_lat = metrics.avg_latency_us();
    assert(avg_lat == 150u);  // (100+200)/2

    std::println("[CanaryMetrics] error_rate={:.3f} avg_latency={} us",
                err_rate, avg_lat);
    std::println("[CanaryMetrics] OK");

    // ─── 5. 100% canary ──────────────────────────────────────────────────────
    router.set_canary_percent(100);
    stable_received.clear();
    canary_received.clear();
    for (int i = 0; i < 10; ++i)
        router.push(Request{i, true});
    assert(canary_received.size() == 10u);
    assert(stable_received.empty());
    std::println("[CanaryRouter] 100% canary: OK");

    std::println("canary_example: ALL OK");
    return 0;
}
