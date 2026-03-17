/**
 * @file pipeline_observer_health_example.cpp
 * @brief 파이프라인 관찰 가능성 + 상태 모니터링 예제.
 *
 * ## 커버리지 — pipeline/observability.hpp
 * - ActionMetrics              — 액션 단위 처리 지표 (items, errors, latency buckets)
 * - ActionMetrics::record_latency_us() — µs 단위 레이턴시 버킷 기록
 * - PipelineMetrics            — 파이프라인 집계 지표
 * - PipelineMetrics::total_processed() / total_errors() / error_rate()
 * - PipelineObserver           — 이벤트 훅 인터페이스
 * - LoggingObserver            — 표준 에러 출력 기본 구현
 * - NoopObserver               — 제로 오버헤드 비활성 구현
 * - HistogramMetrics           — 사용자 정의 버킷 레이턴시 히스토그램
 *
 * ## 커버리지 — pipeline/health.hpp
 * - PipelineVersion            — 시맨틱 버저닝 + compatible_with()
 * - PipelineVersionRegistry    — 파이프라인 버전 레지스트리
 * - ActionHealth               — 액션별 세부 상태 (to_json)
 * - PipelineHealth             — 파이프라인 단위 건강 집계 (recompute, to_json)
 * - HealthStatus               — HEALTHY / DEGRADED / UNHEALTHY
 * - HealthRegistry             — 글로벌 건강 상태 레지스트리
 * - GraphTopologyExporter      — PipelineGraph 토폴로지 JSON 내보내기
 *
 * ## 커버리지 — pipeline/slo.hpp
 * - LatencyHistogram           — 롤링 1024 샘플 p99/p99.9 히스토그램
 * - LatencyHistogram::record() — 레이턴시 샘플 기록
 * - LatencyHistogram::p99() / p999() / bucket_counts()
 * - ErrorBudgetTracker         — 레이턴시 + 에러율 SLO 추적기
 * - ErrorBudgetTracker::record_success() / record_error()
 * - ErrorBudgetTracker::error_rate() / budget_exhausted() / check_slo()
 */

#include <mutex>   // health.hpp uses std::unique_lock — include before health.hpp

#include <qbuem/pipeline/health.hpp>
#include <qbuem/pipeline/observability.hpp>
#include <qbuem/pipeline/slo.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <system_error>

using namespace qbuem;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// §1  ActionMetrics & PipelineMetrics
// ─────────────────────────────────────────────────────────────────────────────

static void demo_action_metrics() {
    std::printf("── §1  ActionMetrics & PipelineMetrics ──\n");

    ActionMetrics m;

    // 아이템 처리 기록 (원자 카운터)
    m.items_processed.fetch_add(100, std::memory_order_relaxed);
    m.errors.fetch_add(3, std::memory_order_relaxed);
    m.retried.fetch_add(5, std::memory_order_relaxed);
    m.dlq_count.fetch_add(1, std::memory_order_relaxed);

    // 레이턴시 버킷 기록 (µs 단위)
    m.record_latency_us(500);    // < 1ms → lat_buckets[0]
    m.record_latency_us(5000);   // < 10ms → lat_buckets[1]
    m.record_latency_us(50000);  // < 100ms → lat_buckets[2]
    m.record_latency_us(200000); // >= 100ms → lat_buckets[3]

    std::printf("  processed=%llu errors=%llu retried=%llu dlq=%llu\n",
                static_cast<unsigned long long>(m.items_processed.load()),
                static_cast<unsigned long long>(m.errors.load()),
                static_cast<unsigned long long>(m.retried.load()),
                static_cast<unsigned long long>(m.dlq_count.load()));
    std::printf("  latency buckets: [0]=%llu [1]=%llu [2]=%llu [3]=%llu\n",
                static_cast<unsigned long long>(m.lat_buckets[0].load()),
                static_cast<unsigned long long>(m.lat_buckets[1].load()),
                static_cast<unsigned long long>(m.lat_buckets[2].load()),
                static_cast<unsigned long long>(m.lat_buckets[3].load()));

    // HistogramMetrics — 사용자 정의 버킷 히스토그램
    auto hist = std::make_shared<HistogramMetrics>(
        std::initializer_list<uint64_t>{1000, 5000, 10000, 50000});
    m.histogram = hist;

    m.record_latency_us(800);    // < 1000 → 버킷 0
    m.record_latency_us(3000);   // < 5000 → 버킷 1
    auto counts = hist->bucket_counts();
    std::printf("  HistogramMetrics 버킷 수: %zu\n", counts.size());

    // m.reset()
    m.reset();
    std::printf("  reset() 후 processed=%llu\n\n",
                static_cast<unsigned long long>(m.items_processed.load()));

    // PipelineMetrics — ActionMetrics는 atomic 필드로 인해 이동 불가
    // actions 벡터는 직접 기록하지 않고 total_processed 등 인터페이스 확인
    PipelineMetrics pm;
    pm.name = "order-pipeline";
    // 빈 상태에서 집계 메서드 확인
    std::printf("  PipelineMetrics '%s': total_processed=%llu error_rate=%.4f\n\n",
                pm.name.c_str(),
                static_cast<unsigned long long>(pm.total_processed()),
                pm.error_rate());
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  PipelineObserver — 이벤트 훅
// ─────────────────────────────────────────────────────────────────────────────

static void demo_observer() {
    std::printf("── §2  PipelineObserver ──\n");

    // LoggingObserver — 기본 로깅 구현
    LoggingObserver logging_obs;
    logging_obs.on_item_done("validate", 42, 1200);
    logging_obs.on_error("enrich",
        std::make_error_code(std::errc::connection_refused));
    logging_obs.on_scale_event("publish", 2, 4);
    logging_obs.on_state_change("order-pipeline", "idle", "running");
    logging_obs.on_circuit_open("payment");
    logging_obs.on_circuit_close("payment");

    // NoopObserver — 제로 오버헤드 비활성 구현
    NoopObserver noop;
    noop.on_item_start("validate", 1);
    noop.on_dlq_item("enrich", {});
    std::printf("  LoggingObserver & NoopObserver 완료\n\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// §3  PipelineVersion & PipelineVersionRegistry
// ─────────────────────────────────────────────────────────────────────────────

static void demo_version() {
    std::printf("── §3  PipelineVersion & PipelineVersionRegistry ──\n");

    PipelineVersion v1{1, 0, 0};
    PipelineVersion v2{1, 2, 3};
    PipelineVersion v3{2, 0, 0};

    std::printf("  v1=%s v2=%s v3=%s\n",
                v1.to_string().c_str(), v2.to_string().c_str(), v3.to_string().c_str());
    std::printf("  v1.compatible_with(v2)=%s (major 동일)\n",
                v1.compatible_with(v2) ? "true" : "false");
    std::printf("  v1.compatible_with(v3)=%s (major 다름)\n",
                v1.compatible_with(v3) ? "true" : "false");

    // PipelineVersionRegistry
    auto& vreg = PipelineVersionRegistry::global();
    vreg.set_version("order-pipeline", v1);
    vreg.set_version("payment-pipeline", v3);

    auto stored = vreg.get("order-pipeline");
    if (stored)
        std::printf("  registered version: %s\n", stored->to_string().c_str());

    bool compat = vreg.compatible_with("order-pipeline", PipelineVersion{1, 5, 0});
    std::printf("  compatible_with(1.5.0): %s\n\n", compat ? "yes" : "no");
}

// ─────────────────────────────────────────────────────────────────────────────
// §4  PipelineHealth & HealthRegistry
// ─────────────────────────────────────────────────────────────────────────────

static void demo_health() {
    std::printf("── §4  PipelineHealth & HealthRegistry ──\n");

    // ActionHealth 직접 구성
    ActionHealth ah_validate;
    ah_validate.name             = "validate";
    ah_validate.circuit_state    = "CLOSED";
    ah_validate.error_rate_1m    = 0.001;
    ah_validate.p99_us           = 800;
    ah_validate.queue_depth      = 0;
    ah_validate.items_processed  = 1000;

    ActionHealth ah_publish;
    ah_publish.name             = "publish";
    ah_publish.circuit_state    = "OPEN";   // 서킷 열림 → DEGRADED
    ah_publish.error_rate_1m    = 0.15;
    ah_publish.p99_us           = 150000;
    ah_publish.queue_depth      = 42;
    ah_publish.items_processed  = 850;

    std::printf("  ActionHealth JSON: %s\n",
                ah_validate.to_json().c_str());

    // PipelineHealth 구성
    PipelineHealth health;
    health.name    = "order-pipeline";
    health.actions = {ah_validate, ah_publish};
    health.recompute();  // status 자동 계산

    std::printf("  전체 상태: %s\n",
                std::string(to_string(health.status)).c_str());

    auto json = health.to_json();
    std::printf("  JSON (첫 80자): %.80s\n", json.c_str());
    if (json.size() > 80) std::printf("  ...\n");

    // HealthRegistry
    auto& hreg = HealthRegistry::global();
    hreg.update(health);

    auto retrieved = hreg.get("order-pipeline");
    if (retrieved)
        std::printf("  HealthRegistry 조회: status=%s\n",
                    std::string(to_string(retrieved->status)).c_str());

    std::printf("  all_json 길이: %zu\n\n", hreg.all_json().size());
}

// ─────────────────────────────────────────────────────────────────────────────
// §5  LatencyHistogram & ErrorBudgetTracker
// ─────────────────────────────────────────────────────────────────────────────

static void demo_slo() {
    std::printf("── §5  LatencyHistogram & ErrorBudgetTracker ──\n");

    // LatencyHistogram — 롤링 1024 샘플
    LatencyHistogram hist;
    for (int i = 0; i < 900; ++i) hist.record(500us);    // 0.5ms
    for (int i = 0; i < 80;  ++i) hist.record(5000us);   // 5ms
    for (int i = 0; i < 15;  ++i) hist.record(50000us);  // 50ms
    for (int i = 0; i < 5;   ++i) hist.record(200000us); // 200ms

    std::printf("  p99=%lldµs p99.9=%lldµs\n",
                static_cast<long long>(hist.p99().count()),
                static_cast<long long>(hist.p999().count()));

    auto buckets = hist.bucket_counts();
    std::printf("  버킷 [<1ms=%llu <10ms=%llu <100ms=%llu >=100ms=%llu]\n",
                static_cast<unsigned long long>(buckets[0]),
                static_cast<unsigned long long>(buckets[1]),
                static_cast<unsigned long long>(buckets[2]),
                static_cast<unsigned long long>(buckets[3]));

    hist.reset();
    std::printf("  reset() 후 p99=%lldµs\n",
                static_cast<long long>(hist.p99().count()));

    // ErrorBudgetTracker — SLO 추적기
    bool violated = false;
    SloConfig cfg;
    cfg.p99_target    = 10000us;    // 10ms
    cfg.p999_target   = 50000us;    // 50ms
    cfg.error_budget  = 0.01;       // 1% 에러 허용
    cfg.on_violation  = [&](std::string_view name) {
        std::printf("  SLO 위반 감지: %.*s\n",
                    static_cast<int>(name.size()), name.data());
        violated = true;
    };

    ErrorBudgetTracker tracker(std::move(cfg), "payment");

    for (int i = 0; i < 980; ++i) tracker.record_success(800us);
    for (int i = 0; i < 20;  ++i) tracker.record_error();   // 2% 에러율

    std::printf("  total=%llu errors=%llu error_rate=%.4f\n",
                static_cast<unsigned long long>(tracker.total_count()),
                static_cast<unsigned long long>(tracker.error_count()),
                tracker.error_rate());
    std::printf("  budget_exhausted=%s\n",
                tracker.budget_exhausted() ? "yes" : "no");

    tracker.check_slo();

    // histogram() 메서드
    auto& inner_hist = tracker.histogram();
    std::printf("  내부 히스토그램 p99=%lldµs\n\n",
                static_cast<long long>(inner_hist.p99().count()));
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::printf("=== qbuem 파이프라인 관찰 가능성 + SLO 예제 ===\n\n");

    demo_action_metrics();
    demo_observer();
    demo_version();
    demo_health();
    demo_slo();

    std::printf("=== 완료 ===\n");
    return 0;
}
