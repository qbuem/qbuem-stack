/**
 * @file io_metrics_dashboard.cpp
 * @brief 초고속 IO 기반 실시간 데이터 메트릭스 대시보드
 *
 * ## 개요
 * qbuem의 lock-free 채널, StaticPipeline, MessageBus, HistogramMetrics를
 * 조합해 고처리량 IO 이벤트 스트림을 실시간으로 집계·출력하는
 * 데이터 메트릭스 대시보드를 구현합니다.
 *
 * ## 아키텍처
 * ```
 * ┌──────────── IO Event Producers (4 스레드, ~100k ev/s) ─────────────┐
 * │  ProducerThread × 4  →  AsyncChannel<IOEvent>  (lock-free MPMC)   │
 * └──────────────────────────┬─────────────────────────────────────────┘
 *                             │ IOEvent (Read/Write/Accept/Connect/Error)
 *                             ▼
 *         ┌──────────────────────────────────────────────────┐
 *         │  StaticPipeline<IOEvent, MetricSnapshot>          │
 *         │  1. classify()    — 이벤트 타입 분류 + 바이트 계산  │
 *         │  2. measure()     — 레이턴시 기록 (HistogramMetrics) │
 *         │  3. aggregate()   — EWMA 처리량 + 슬라이딩 윈도우   │
 *         │  4. snapshot()    — MetricSnapshot 생성             │
 *         └──────────────────┬───────────────────────────────┘
 *                             │ MetricSnapshot
 *                      MessageBus("metrics")
 *                             │
 *                  DashboardPrinter (구독자)
 *                  AlertChecker     (구독자)
 *
 * ## 메트릭 항목
 * - 처리량     : ops/s (EWMA α=0.1), bytes/s (rolling 1s 윈도우)
 * - 레이턴시   : p50 / p95 / p99 / p999 (HDR-style HistogramMetrics)
 * - 에러율     : error/total × 100%
 * - 큐 깊이    : AsyncChannel 점유율
 * - 이벤트 유형: Read / Write / Accept / Connect / Close / Error 비율
 *
 * ## 커버리지
 * - HistogramMetrics (observability.hpp) — p50/p95/p99 레이턴시 분포
 * - StaticPipeline<In, Out> + PipelineBuilder + with_sink()
 * - MessageBus: publish/subscribe (메트릭 fan-out)
 * - AsyncChannel<T>: MPMC 락-프리 큐
 * - std::atomic 카운터 + EWMA 처리량
 * - 4개 Producer 스레드 동시 실행
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/async_channel.hpp>
#include <qbuem/pipeline/message_bus.hpp>
#include <qbuem/pipeline/observability.hpp>
#include <qbuem/pipeline/static_pipeline.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// ─────────────────────────────────────────────────────────────────────────────
// §0  IO 이벤트 타입
// ─────────────────────────────────────────────────────────────────────────────

enum class IOEventType : uint8_t {
    Read    = 0,
    Write   = 1,
    Accept  = 2,
    Connect = 3,
    Close   = 4,
    Error   = 5,
    _Count  = 6,
};

static const char* event_name(IOEventType t) {
    switch (t) {
    case IOEventType::Read:    return "READ   ";
    case IOEventType::Write:   return "WRITE  ";
    case IOEventType::Accept:  return "ACCEPT ";
    case IOEventType::Connect: return "CONNECT";
    case IOEventType::Close:   return "CLOSE  ";
    case IOEventType::Error:   return "ERROR  ";
    default:                   return "?      ";
    }
}

struct IOEvent {
    IOEventType type{IOEventType::Read};
    uint32_t    fd{0};
    uint32_t    bytes{0};        ///< 전송 바이트 (Read/Write)
    uint32_t    latency_us{0};   ///< 완료 레이턴시 (µs)
    uint8_t     errno_code{0};   ///< Error 이벤트 시 errno
    uint64_t    ts_us{0};        ///< 이벤트 발생 시각 (µs)
};

// ─────────────────────────────────────────────────────────────────────────────
// §1  메트릭 스냅샷 (1초 집계 단위)
// ─────────────────────────────────────────────────────────────────────────────

struct MetricSnapshot {
    uint64_t  total_ops{0};
    uint64_t  total_bytes{0};
    uint64_t  error_count{0};
    double    ops_per_sec{0};       ///< EWMA 처리량
    double    bytes_per_sec{0};
    double    error_rate_pct{0};

    // 레이턴시 (µs)
    uint64_t  lat_p50{0};
    uint64_t  lat_p95{0};
    uint64_t  lat_p99{0};
    uint64_t  lat_p999{0};
    uint64_t  lat_max{0};

    // 이벤트 유형별 카운트
    uint64_t  type_count[static_cast<int>(IOEventType::_Count)]{};

    uint64_t  snapshot_id{0};
    uint64_t  ts_us{0};
};

// ─────────────────────────────────────────────────────────────────────────────
// §2  글로벌 집계 상태 (lock-free)
// ─────────────────────────────────────────────────────────────────────────────

// 캐시라인 패딩 카운터 — classify() 가 여러 워커에서 동시에 다른 type_count 버킷을
// 증가시킬 때 false sharing 을 방지합니다 (6개 × 8 B = 48 B → 2 캐시라인 공유).
struct alignas(64) PaddedCounter {
    std::atomic<uint64_t> val{0};
};

struct GlobalStats {
    // 전체 카운터
    std::atomic<uint64_t> total_ops{0};
    std::atomic<uint64_t> total_bytes{0};
    std::atomic<uint64_t> error_count{0};

    // 이벤트 유형별 카운터 — 각 버킷이 독립 캐시라인 점유
    PaddedCounter type_count[static_cast<int>(IOEventType::_Count)]{};

    // EWMA 처리량 (α = 0.15) — ewma_mtx 로 보호
    // build_snapshot() 이 멀티워커 파이프라인에서 동시 호출될 수 있으므로 mutex 사용
    mutable std::mutex ewma_mtx;
    double ewma_ops{0};
    double ewma_bytes{0};

    // 슬라이딩 1초 윈도우
    std::atomic<uint64_t> window_ops{0};
    std::atomic<uint64_t> window_bytes{0};
    // 비원자 time_point 대신 ns 단위 정수 atomic — ewma_mtx 보호 구간에서만 읽고 씀
    uint64_t window_start_ns{static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count())};

    // 레이턴시 히스토그램 (1µs ~ 1s, 로그 스케일 버킷)
    // 버킷 경계: 10, 50, 100, 500, 1000, 5000, 10000, 50000, 100000, 500000 µs
    HistogramMetrics latency_hist{
        {10, 50, 100, 500, 1000, 5000, 10000, 50000, 100000, 500000}
    };

    // 최대 레이턴시
    std::atomic<uint64_t> lat_max{0};

    // 스냅샷 ID
    std::atomic<uint64_t> snap_id{0};
};

static GlobalStats g_stats{};

// ─────────────────────────────────────────────────────────────────────────────
// §3  백분위수 계산 (HistogramMetrics 버킷에서)
// ─────────────────────────────────────────────────────────────────────────────

// 버킷 경계: 10, 50, 100, 500, 1k, 5k, 10k, 50k, 100k, 500k µs → 11개 버킷
static const uint64_t kBounds[] = {10, 50, 100, 500, 1000, 5000, 10000, 50000, 100000, 500000};
static const uint64_t kBoundMids[] = {5, 30, 75, 300, 750, 3000, 7500, 30000, 75000, 300000, 750000};

// 4개 백분위를 버킷 배열 단일 패스로 동시 계산 — O(n) × 4 → O(n) × 1
// 스택 배열(counts_buf)을 받아 벡터 할당도 없앰
static void compute_percentiles(const uint64_t* counts, size_t n,
                                 uint64_t& p50, uint64_t& p95,
                                 uint64_t& p99, uint64_t& p999) noexcept {
    uint64_t total = 0;
    for (size_t i = 0; i < n; ++i) total += counts[i];
    p50 = p95 = p99 = p999 = 0;
    if (total == 0) return;

    constexpr double kPs[4] = {50.0, 95.0, 99.0, 99.9};
    uint64_t targets[4];
    for (int j = 0; j < 4; ++j)
        targets[j] = static_cast<uint64_t>(std::ceil(total * kPs[j] / 100.0));

    uint64_t* results[4] = {&p50, &p95, &p99, &p999};
    uint64_t acc = 0;
    int found = 0;
    for (size_t i = 0; i < n && found < 4; ++i) {
        acc += counts[i];
        while (found < 4 && acc >= targets[found]) {
            *results[found] = (i < (sizeof(kBoundMids)/sizeof(kBoundMids[0])))
                ? kBoundMids[i] : 1000000ULL;
            ++found;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// §4  StaticPipeline 스테이지
// ─────────────────────────────────────────────────────────────────────────────

// Stage 1: 이벤트 분류 + 바이트/에러 전역 집계
static Task<Result<IOEvent>> classify(IOEvent ev, ActionEnv) {
    int idx = static_cast<int>(ev.type);
    g_stats.type_count[idx].val.fetch_add(1, std::memory_order_relaxed);
    g_stats.total_ops.fetch_add(1, std::memory_order_relaxed);
    if (ev.type == IOEventType::Read || ev.type == IOEventType::Write)
        g_stats.total_bytes.fetch_add(ev.bytes, std::memory_order_relaxed);
    if (ev.type == IOEventType::Error)
        g_stats.error_count.fetch_add(1, std::memory_order_relaxed);
    // 슬라이딩 윈도우
    g_stats.window_ops.fetch_add(1, std::memory_order_relaxed);
    g_stats.window_bytes.fetch_add(ev.bytes, std::memory_order_relaxed);
    co_return ev;
}

// Stage 2: 레이턴시 기록
static Task<Result<IOEvent>> measure(IOEvent ev, ActionEnv) {
    g_stats.latency_hist.observe(ev.latency_us);
    // 최대 레이턴시: 단일 CAS — CAS 실패 시 이미 더 큰 값이 들어온 것이므로 재시도 불필요
    uint64_t cur = g_stats.lat_max.load(std::memory_order_relaxed);
    if (ev.latency_us > cur)
        g_stats.lat_max.compare_exchange_strong(
            cur, ev.latency_us,
            std::memory_order_relaxed, std::memory_order_relaxed);
    co_return ev;
}

// Stage 3: EWMA 처리량 계산 + 스냅샷 트리거 여부 결정
static std::atomic<uint64_t> g_processed_since_snap{0};

static Task<Result<IOEvent>> aggregate(IOEvent ev, ActionEnv) {
    g_processed_since_snap.fetch_add(1, std::memory_order_relaxed);
    co_return ev;
}

// ─────────────────────────────────────────────────────────────────────────────
// §4b  Forward declarations (정의는 §6에)
// ─────────────────────────────────────────────────────────────────────────────
static std::atomic<int> g_snap_received{0};
static void print_dashboard(const MetricSnapshot& s);

// ─────────────────────────────────────────────────────────────────────────────
// §4c  스냅샷 조립 헬퍼 (non-coroutine, 코루틴 프레임 외부에서 실행)
// ─────────────────────────────────────────────────────────────────────────────

[[gnu::noinline]]
static MetricSnapshot build_snapshot() {
    MetricSnapshot snap;
    snap.snapshot_id  = g_stats.snap_id.fetch_add(1, std::memory_order_relaxed);
    snap.total_ops    = g_stats.total_ops.load(std::memory_order_relaxed);
    snap.total_bytes  = g_stats.total_bytes.load(std::memory_order_relaxed);
    snap.error_count  = g_stats.error_count.load(std::memory_order_relaxed);

    // 슬라이딩 윈도우 처리량 (EWMA) — ewma_mtx 로 race 방지
    // (pipeline 멀티워커가 동시에 build_snapshot 호출 가능)
    {
        auto now_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now().time_since_epoch()).count());
        std::lock_guard<std::mutex> lk(g_stats.ewma_mtx);
        double elapsed = static_cast<double>(now_ns - g_stats.window_start_ns) * 1e-9;
        if (elapsed > 0.01) {
            uint64_t wops   = g_stats.window_ops.exchange(0, std::memory_order_relaxed);
            uint64_t wbytes = g_stats.window_bytes.exchange(0, std::memory_order_relaxed);
            constexpr double kAlpha = 0.15;
            g_stats.ewma_ops   += kAlpha * (wops   / elapsed - g_stats.ewma_ops);
            g_stats.ewma_bytes += kAlpha * (wbytes / elapsed - g_stats.ewma_bytes);
            g_stats.window_start_ns = now_ns;
        }
        snap.ops_per_sec   = g_stats.ewma_ops;
        snap.bytes_per_sec = g_stats.ewma_bytes;
    }

    if (snap.total_ops > 0)
        snap.error_rate_pct = 100.0 * snap.error_count / snap.total_ops;

    // fill_bucket_counts: 스택 버퍼 사용 — 벡터 힙 할당 없이 버킷 읽기
    // bucket_count() = 11 (경계 10개 + overflow 1개)
    constexpr size_t kMaxBuckets = 16;
    uint64_t counts_buf[kMaxBuckets]{};
    size_t nbuckets = g_stats.latency_hist.fill_bucket_counts(counts_buf, kMaxBuckets);

    // 4개 백분위 단일 패스 계산
    compute_percentiles(counts_buf, nbuckets,
                        snap.lat_p50, snap.lat_p95, snap.lat_p99, snap.lat_p999);
    snap.lat_max = g_stats.lat_max.load(std::memory_order_relaxed);

    for (int i = 0; i < static_cast<int>(IOEventType::_Count); ++i)
        snap.type_count[i] = g_stats.type_count[i].val.load(std::memory_order_relaxed);

    return snap;
}

// Stage 4: Sink — N번째 이벤트마다 스냅샷 생성 + 대시보드 출력 + MessageBus 발행
static std::atomic<uint64_t> g_snap_trigger{0};
static constexpr uint64_t kSnapInterval = 500;   ///< 500 이벤트마다 스냅샷

struct SnapSink {
    MessageBus* bus{nullptr};

    Result<void> init() { return {}; }

    Task<Result<void>> sink(const IOEvent& /*ev*/) {
        uint64_t seq = g_snap_trigger.fetch_add(1, std::memory_order_relaxed);
        if ((seq % kSnapInterval) != 0) {
            co_return Result<void>{};
        }

        // 스냅샷 조립 (helper 함수로 분리 — GCC coroutine 프레임 크기 최소화)
        MetricSnapshot snap = build_snapshot();

        // 직접 대시보드 출력
        print_dashboard(snap);
        g_snap_received.fetch_add(1, std::memory_order_relaxed);

        // MessageBus fan-out: 경보 구독자에게도 전달
        if (bus) co_await bus->publish("alerts", snap);
        co_return Result<void>{};
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// §5  IO 이벤트 Mock 생성기 (4개 Producer 스레드)
// ─────────────────────────────────────────────────────────────────────────────

struct MockIOProducer {
    int              thread_id{0};
    int              target_ops{0};   ///< 생성할 이벤트 수
    static constexpr double kErrorRate = 0.02;  ///< 2% 에러 비율

    void run(StaticPipeline<IOEvent, IOEvent>& pipe) {
        std::mt19937 rng(static_cast<uint32_t>(thread_id * 1234567));
        // 레이턴시 분포: 대부분 50~500µs, 꼬리 분포 있음
        std::gamma_distribution<double> lat_dist(2.0, 150.0);    // 평균 300µs
        std::uniform_int_distribution<int> type_dist(0, 5);
        std::uniform_int_distribution<uint32_t> bytes_dist(64, 65536);
        std::uniform_int_distribution<uint32_t> fd_dist(3, 65535);

        uint64_t ts = static_cast<uint64_t>(thread_id) * 1000000ULL;

        for (int i = 0; i < target_ops; ++i) {
            IOEventType type;
            double r01 = static_cast<double>(rng()) / static_cast<double>(rng.max());
            if (r01 < kErrorRate) {
                type = IOEventType::Error;
            } else {
                // Read 40%, Write 35%, Accept 10%, Connect 8%, Close 7%
                double p = static_cast<double>(rng()) / static_cast<double>(rng.max());
                if      (p < 0.40) type = IOEventType::Read;
                else if (p < 0.75) type = IOEventType::Write;
                else if (p < 0.85) type = IOEventType::Accept;
                else if (p < 0.93) type = IOEventType::Connect;
                else               type = IOEventType::Close;
            }

            uint32_t lat_us = static_cast<uint32_t>(
                std::clamp(lat_dist(rng), 1.0, 800000.0));
            uint32_t bytes = (type == IOEventType::Read || type == IOEventType::Write)
                ? bytes_dist(rng) : 0;

            IOEvent ev{
                .type       = type,
                .fd         = fd_dist(rng),
                .bytes      = bytes,
                .latency_us = lat_us,
                .errno_code = (type == IOEventType::Error)
                    ? static_cast<uint8_t>(rng() % 10u + 1u) : uint8_t{0},
                .ts_us      = ts,
            };

            // try_push (lock-free MPMC): 채널 포화 시 지수 백오프 재시도 (최대 8회)
            // 고정 8µs 반복 대신 백오프로 불필요한 컨텍스트 스위치 절감
            {
                uint32_t backoff_us = 4;
                for (int retry = 0; retry < 8; ++retry) {
                    if (pipe.try_push(ev)) break;
                    std::this_thread::sleep_for(std::chrono::microseconds(backoff_us));
                    backoff_us = std::min(backoff_us * 2, uint32_t{256});
                }
            }
            ts += 10 + (rng() % 40);  // 10~50µs 간격
            // 주기 pacing sleep 제거: 파이프라인은 backpressure 로 자율 조절
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// §6  대시보드 렌더러
// ─────────────────────────────────────────────────────────────────────────────

static void print_dashboard(const MetricSnapshot& s) {
    std::printf(
        "\n╔═══════════════════════════════════════════════════════════╗\n"
        "║  IO 메트릭스 대시보드  [snap #%llu]\n"
        "╠═══════════════════════════════════════════════════════════╣\n"
        "║  처리량    : %8.0f ops/s  |  %8.2f MB/s\n"
        "║  총 이벤트 : %llu ops  |  에러율 %.2f%%\n"
        "╠═════════════════ 레이턴시 분포 ═══════════════════════════╣\n"
        "║  p50   : %6llu µs  (%5.2f ms)\n"
        "║  p95   : %6llu µs  (%5.2f ms)\n"
        "║  p99   : %6llu µs  (%5.2f ms)\n"
        "║  p99.9 : %6llu µs  (%5.2f ms)\n"
        "║  max   : %6llu µs  (%5.2f ms)\n"
        "╠═════════════════ 이벤트 유형 비율 ════════════════════════╣\n",
        (unsigned long long)s.snapshot_id,
        s.ops_per_sec, s.bytes_per_sec / (1024.0*1024.0),
        (unsigned long long)s.total_ops, s.error_rate_pct,
        (unsigned long long)s.lat_p50,  s.lat_p50  / 1000.0,
        (unsigned long long)s.lat_p95,  s.lat_p95  / 1000.0,
        (unsigned long long)s.lat_p99,  s.lat_p99  / 1000.0,
        (unsigned long long)s.lat_p999, s.lat_p999 / 1000.0,
        (unsigned long long)s.lat_max,  s.lat_max  / 1000.0);

    uint64_t total_ops = std::max(s.total_ops, uint64_t{1});
    for (int i = 0; i < static_cast<int>(IOEventType::_Count); ++i) {
        double pct = 100.0 * s.type_count[i] / total_ops;
        int bar = static_cast<int>(pct / 5.0);  // 5% per '#'
        std::printf("║  %s : %6llu  (%5.1f%%)  ",
                    event_name(static_cast<IOEventType>(i)),
                    (unsigned long long)s.type_count[i], pct);
        for (int b = 0; b < bar; ++b) std::putchar('#');
        std::putchar('\n');
    }
    std::printf("╚═══════════════════════════════════════════════════════════╝\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// §7  메인
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::printf("=== 초고속 IO 기반 실시간 데이터 메트릭스 대시보드 ===\n");
    std::printf("    4 Producer 스레드 × 20,000 이벤트 = 80,000 IO 이벤트\n");
    std::printf("    StaticPipeline: classify → measure → aggregate → snap\n");
    std::printf("    MessageBus: metrics 토픽으로 스냅샷 fan-out\n\n");

    constexpr int kProducers       = 4;
    constexpr int kEventsPerThread = 20000;  // 총 80,000 이벤트

    Dispatcher disp(4);
    std::jthread worker([&] { disp.run(); });

    // ── §A  MessageBus 설정 — "alerts" 토픽으로 경보 fan-out ──────────────────
    MessageBus bus;
    bus.start(disp);

    // 경보 구독자: p99 > 50ms 또는 에러율 > 5% 시 경보 출력
    auto sub_alert = bus.subscribe("alerts",
        [](MessageBus::Msg msg, Context) -> Task<Result<void>> {
            try {
                auto& snap = std::any_cast<MetricSnapshot&>(msg);
                if (snap.lat_p99 > 50000)
                    std::printf("  [경보] p99 레이턴시 %.1fms 초과!\n",
                                snap.lat_p99 / 1000.0);
                if (snap.error_rate_pct > 5.0)
                    std::printf("  [경보] 에러율 %.2f%% 임계값 초과!\n",
                                snap.error_rate_pct);
            } catch (...) {}
            co_return Result<void>{};
        });

    // ── §B  StaticPipeline 구성 ──────────────────────────────────────────────
    std::printf("── §A  IO 메트릭 StaticPipeline 구성 ──\n\n");

    SnapSink sink_obj;
    sink_obj.bus = &bus;

    auto pipe = std::make_shared<StaticPipeline<IOEvent, IOEvent>>(
        PipelineBuilder<IOEvent>{}
            .add<IOEvent>(classify)
            .add<IOEvent>(measure)
            .add<IOEvent>(aggregate)
            .with_sink(std::move(sink_obj))
            .build());
    pipe->start(disp);

    // ── §C  4개 Producer 스레드 동시 실행 ────────────────────────────────────
    std::printf("── §B  IO 이벤트 생성 시작 (%d 스레드 × %d 이벤트) ──\n\n",
                kProducers, kEventsPerThread);

    auto t_start = Clock::now();

    std::vector<std::jthread> producers;
    producers.reserve(kProducers);
    for (int t = 0; t < kProducers; ++t) {
        producers.emplace_back([&, t]() {
            MockIOProducer prod;
            prod.thread_id  = t;
            prod.target_ops = kEventsPerThread;
            prod.run(*pipe);
        });
    }
    for (auto& th : producers) th.join();

    auto t_produce = Clock::now();
    double produce_sec = std::chrono::duration<double>(t_produce - t_start).count();
    std::printf("\n  [생성 완료] %.3f초, %.0f ops/s (실제 생성 처리량)\n",
                produce_sec,
                (kProducers * kEventsPerThread) / produce_sec);

    // 파이프라인 처리 완료 대기
    std::this_thread::sleep_for(1s);

    disp.stop();
    worker.join();

    // ── §D  최종 결과 요약 ────────────────────────────────────────────────────
    uint64_t final_ops    = g_stats.total_ops.load(std::memory_order_relaxed);
    uint64_t final_bytes  = g_stats.total_bytes.load(std::memory_order_relaxed);
    uint64_t final_errors = g_stats.error_count.load(std::memory_order_relaxed);
    // 최종 요약: fill_bucket_counts 로 heap 할당 없이 읽기
    constexpr size_t kSummaryBuckets = 16;
    uint64_t summary_buf[kSummaryBuckets]{};
    size_t summary_n = g_stats.latency_hist.fill_bucket_counts(summary_buf, kSummaryBuckets);
    uint64_t fp50{}, fp95{}, fp99{}, fp999{};
    compute_percentiles(summary_buf, summary_n, fp50, fp95, fp99, fp999);

    std::printf("\n══════════════════════════════════════════════════════\n");
    std::printf("  IO 메트릭스 최종 요약\n");
    std::printf("══════════════════════════════════════════════════════\n");
    std::printf("  총 이벤트     : %llu\n",   (unsigned long long)final_ops);
    std::printf("  총 데이터     : %.2f MB\n", final_bytes / (1024.0*1024.0));
    std::printf("  에러 수       : %llu (%.2f%%)\n",
                (unsigned long long)final_errors,
                final_ops > 0 ? 100.0 * final_errors / final_ops : 0.0);
    std::printf("  스냅샷 수신   : %d 회\n", g_snap_received.load());
    std::printf("  p50 레이턴시  : %llu µs\n",  (unsigned long long)fp50);
    std::printf("  p95 레이턴시  : %llu µs\n",  (unsigned long long)fp95);
    std::printf("  p99 레이턴시  : %llu µs\n",  (unsigned long long)fp99);
    std::printf("  p99.9 레이턴시: %llu µs\n",  (unsigned long long)fp999);
    std::printf("  최대 레이턴시 : %llu µs\n",
                (unsigned long long)g_stats.lat_max.load(std::memory_order_relaxed));
    std::printf("──────────────────────────────────────────────────────\n");

    // 이벤트 유형 분포
    std::printf("  이벤트 분포:\n");
    for (int i = 0; i < static_cast<int>(IOEventType::_Count); ++i) {
        uint64_t c = g_stats.type_count[i].val.load(std::memory_order_relaxed);
        double   p = final_ops > 0 ? 100.0 * c / final_ops : 0.0;
        std::printf("    %s %6llu  (%.1f%%)\n",
                    event_name(static_cast<IOEventType>(i)),
                    (unsigned long long)c, p);
    }

    bool ok = (final_ops > 0 && g_snap_received.load() > 0);
    std::printf("\nio_metrics_dashboard: %s\n", ok ? "ALL OK" : "WARN — 출력 없음");
    return 0;
}
