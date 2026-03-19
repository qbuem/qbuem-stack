/**
 * @file io_metrics_dashboard.cpp
 * @brief High-speed IO-based real-time data metrics dashboard
 *
 * ## Overview
 * Combines qbuem's lock-free channel, StaticPipeline, MessageBus, and
 * HistogramMetrics to implement a real-time data metrics dashboard that
 * aggregates and displays a high-throughput IO event stream.
 *
 * ## Architecture
 * ```
 * ┌──────────── IO Event Producers (4 threads, ~100k ev/s) ────────────┐
 * │  ProducerThread × 4  →  AsyncChannel<IOEvent>  (lock-free MPMC)   │
 * └──────────────────────────┬─────────────────────────────────────────┘
 *                             │ IOEvent (Read/Write/Accept/Connect/Error)
 *                             ▼
 *         ┌──────────────────────────────────────────────────┐
 *         │  StaticPipeline<IOEvent, MetricSnapshot>          │
 *         │  1. classify()    — event type classification + byte counting  │
 *         │  2. measure()     — latency recording (HistogramMetrics) │
 *         │  3. aggregate()   — EWMA throughput + sliding window   │
 *         │  4. snapshot()    — MetricSnapshot generation             │
 *         └──────────────────┬───────────────────────────────┘
 *                             │ MetricSnapshot
 *                      MessageBus("metrics")
 *                             │
 *                  DashboardPrinter (subscriber)
 *                  AlertChecker     (subscriber)
 *
 * ## Metric fields
 * - Throughput   : ops/s (EWMA α=0.1), bytes/s (rolling 1s window)
 * - Latency      : p50 / p95 / p99 / p999 (HDR-style HistogramMetrics)
 * - Error rate   : error/total × 100%
 * - Queue depth  : AsyncChannel occupancy
 * - Event types  : Read / Write / Accept / Connect / Close / Error distribution
 *
 * ## Coverage
 * - HistogramMetrics (observability.hpp) — p50/p95/p99 latency distribution
 * - StaticPipeline<In, Out> + PipelineBuilder + with_sink()
 * - MessageBus: publish/subscribe (metrics fan-out)
 * - AsyncChannel<T>: MPMC lock-free queue
 * - std::atomic counters + EWMA throughput
 * - 4 producer threads running concurrently
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
#include <print>
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
// §0  IO event types
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
    uint32_t    bytes{0};        ///< transferred bytes (Read/Write)
    uint32_t    latency_us{0};   ///< completion latency (µs)
    uint8_t     errno_code{0};   ///< errno for Error events
    uint64_t    ts_us{0};        ///< event timestamp (µs)
};

// ─────────────────────────────────────────────────────────────────────────────
// §1  Metric snapshot (1-second aggregation unit)
// ─────────────────────────────────────────────────────────────────────────────

struct MetricSnapshot {
    uint64_t  total_ops{0};
    uint64_t  total_bytes{0};
    uint64_t  error_count{0};
    double    ops_per_sec{0};       ///< EWMA throughput
    double    bytes_per_sec{0};
    double    error_rate_pct{0};

    // latency (µs)
    uint64_t  lat_p50{0};
    uint64_t  lat_p95{0};
    uint64_t  lat_p99{0};
    uint64_t  lat_p999{0};
    uint64_t  lat_max{0};

    // per-event-type counts
    uint64_t  type_count[static_cast<int>(IOEventType::_Count)]{};

    uint64_t  snapshot_id{0};
    uint64_t  ts_us{0};
};

// ─────────────────────────────────────────────────────────────────────────────
// §2  Global aggregation state (lock-free)
// ─────────────────────────────────────────────────────────────────────────────

// Cache-line-padded counter — prevents false sharing when classify() increments
// different type_count buckets concurrently across multiple workers
// (6 × 8B = 48B → 2 shared cache lines).
struct alignas(64) PaddedCounter {
    std::atomic<uint64_t> val{0};
};

struct GlobalStats {
    // total counters
    std::atomic<uint64_t> total_ops{0};
    std::atomic<uint64_t> total_bytes{0};
    std::atomic<uint64_t> error_count{0};

    // per-event-type counters — each bucket occupies an independent cache line
    PaddedCounter type_count[static_cast<int>(IOEventType::_Count)]{};

    // EWMA throughput (α = 0.15) — protected by ewma_mtx
    // build_snapshot() may be called concurrently from multi-worker pipeline, so mutex is used
    mutable std::mutex ewma_mtx;
    double ewma_ops{0};
    double ewma_bytes{0};

    // sliding 1-second window
    std::atomic<uint64_t> window_ops{0};
    std::atomic<uint64_t> window_bytes{0};
    // integer atomic in ns instead of non-atomic time_point — read/written only within ewma_mtx protected section
    uint64_t window_start_ns{static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count())};

    // latency histogram (1µs ~ 1s, log-scale buckets)
    // bucket boundaries: 10, 50, 100, 500, 1000, 5000, 10000, 50000, 100000, 500000 µs
    HistogramMetrics latency_hist{
        {10, 50, 100, 500, 1000, 5000, 10000, 50000, 100000, 500000}
    };

    // maximum latency
    std::atomic<uint64_t> lat_max{0};

    // snapshot ID
    std::atomic<uint64_t> snap_id{0};
};

static GlobalStats g_stats{};

// ─────────────────────────────────────────────────────────────────────────────
// §3  Percentile computation (from HistogramMetrics buckets)
// ─────────────────────────────────────────────────────────────────────────────

// bucket boundaries: 10, 50, 100, 500, 1k, 5k, 10k, 50k, 100k, 500k µs → 11 buckets
static const uint64_t kBounds[] = {10, 50, 100, 500, 1000, 5000, 10000, 50000, 100000, 500000};
static const uint64_t kBoundMids[] = {5, 30, 75, 300, 750, 3000, 7500, 30000, 75000, 300000, 750000};

// compute 4 percentiles in a single pass over bucket array — O(n) × 4 → O(n) × 1
// receives stack-allocated counts_buf to avoid vector heap allocation
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
// §4  StaticPipeline stages
// ─────────────────────────────────────────────────────────────────────────────

// Stage 1: event classification + global byte/error accumulation
static Task<Result<IOEvent>> classify(IOEvent ev, ActionEnv) {
    int idx = static_cast<int>(ev.type);
    g_stats.type_count[idx].val.fetch_add(1, std::memory_order_relaxed);
    g_stats.total_ops.fetch_add(1, std::memory_order_relaxed);
    if (ev.type == IOEventType::Read || ev.type == IOEventType::Write)
        g_stats.total_bytes.fetch_add(ev.bytes, std::memory_order_relaxed);
    if (ev.type == IOEventType::Error)
        g_stats.error_count.fetch_add(1, std::memory_order_relaxed);
    // sliding window
    g_stats.window_ops.fetch_add(1, std::memory_order_relaxed);
    g_stats.window_bytes.fetch_add(ev.bytes, std::memory_order_relaxed);
    co_return ev;
}

// Stage 2: latency recording
static Task<Result<IOEvent>> measure(IOEvent ev, ActionEnv) {
    g_stats.latency_hist.observe(ev.latency_us);
    // max latency: single CAS — no retry needed since CAS failure means a larger value was already written
    uint64_t cur = g_stats.lat_max.load(std::memory_order_relaxed);
    if (ev.latency_us > cur)
        g_stats.lat_max.compare_exchange_strong(
            cur, ev.latency_us,
            std::memory_order_relaxed, std::memory_order_relaxed);
    co_return ev;
}

// Stage 3: EWMA throughput calculation + snapshot trigger check
static std::atomic<uint64_t> g_processed_since_snap{0};

static Task<Result<IOEvent>> aggregate(IOEvent ev, ActionEnv) {
    g_processed_since_snap.fetch_add(1, std::memory_order_relaxed);
    co_return ev;
}

// ─────────────────────────────────────────────────────────────────────────────
// §4b  Forward declarations (defined in §6)
// ─────────────────────────────────────────────────────────────────────────────
static std::atomic<int> g_snap_received{0};
static void print_dashboard(const MetricSnapshot& s);

// ─────────────────────────────────────────────────────────────────────────────
// §4c  Snapshot assembly helper (non-coroutine, runs outside the coroutine frame)
// ─────────────────────────────────────────────────────────────────────────────

[[gnu::noinline]]
static MetricSnapshot build_snapshot() {
    MetricSnapshot snap;
    snap.snapshot_id  = g_stats.snap_id.fetch_add(1, std::memory_order_relaxed);
    snap.total_ops    = g_stats.total_ops.load(std::memory_order_relaxed);
    snap.total_bytes  = g_stats.total_bytes.load(std::memory_order_relaxed);
    snap.error_count  = g_stats.error_count.load(std::memory_order_relaxed);

    // sliding window throughput (EWMA) — ewma_mtx prevents data race
    // (pipeline multi-workers may call build_snapshot concurrently)
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

    // fill_bucket_counts: uses stack buffer — reads buckets without heap allocation
    // bucket_count() = 11 (10 boundaries + 1 overflow)
    constexpr size_t kMaxBuckets = 16;
    uint64_t counts_buf[kMaxBuckets]{};
    size_t nbuckets = g_stats.latency_hist.fill_bucket_counts(counts_buf, kMaxBuckets);

    // compute 4 percentiles in a single pass
    compute_percentiles(counts_buf, nbuckets,
                        snap.lat_p50, snap.lat_p95, snap.lat_p99, snap.lat_p999);
    snap.lat_max = g_stats.lat_max.load(std::memory_order_relaxed);

    for (int i = 0; i < static_cast<int>(IOEventType::_Count); ++i)
        snap.type_count[i] = g_stats.type_count[i].val.load(std::memory_order_relaxed);

    return snap;
}

// Stage 4: Sink — generate snapshot every N events + print dashboard + publish to MessageBus
static std::atomic<uint64_t> g_snap_trigger{0};
static constexpr uint64_t kSnapInterval = 500;   ///< snapshot every 500 events

struct SnapSink {
    MessageBus* bus{nullptr};

    Result<void> init() { return {}; }

    Task<Result<void>> sink(const IOEvent& /*ev*/) {
        uint64_t seq = g_snap_trigger.fetch_add(1, std::memory_order_relaxed);
        if ((seq % kSnapInterval) != 0) {
            co_return Result<void>{};
        }

        // assemble snapshot (split into helper function to minimize GCC coroutine frame size)
        MetricSnapshot snap = build_snapshot();

        // print dashboard directly
        print_dashboard(snap);
        g_snap_received.fetch_add(1, std::memory_order_relaxed);

        // MessageBus fan-out: also deliver to alert subscribers
        if (bus) co_await bus->publish("alerts", snap);
        co_return Result<void>{};
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// §5  IO event mock generator (4 producer threads)
// ─────────────────────────────────────────────────────────────────────────────

struct MockIOProducer {
    int              thread_id{0};
    int              target_ops{0};   ///< number of events to generate
    static constexpr double kErrorRate = 0.02;  ///< 2% error rate

    void run(StaticPipeline<IOEvent, IOEvent>& pipe) {
        std::mt19937 rng(static_cast<uint32_t>(thread_id * 1234567));
        // latency distribution: mostly 50~500µs with tail distribution
        std::gamma_distribution<double> lat_dist(2.0, 150.0);    // mean ~300µs
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

            // try_push (lock-free MPMC): exponential backoff retry on channel saturation (max 8 retries)
            // backoff instead of fixed 8µs spin reduces unnecessary context switches
            {
                uint32_t backoff_us = 4;
                for (int retry = 0; retry < 8; ++retry) {
                    if (pipe.try_push(ev)) break;
                    std::this_thread::sleep_for(std::chrono::microseconds(backoff_us));
                    backoff_us = std::min(backoff_us * 2, uint32_t{256});
                }
            }
            ts += 10 + (rng() % 40);  // 10~50µs interval
            // periodic pacing sleep removed: pipeline self-regulates via backpressure
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// §6  Dashboard renderer
// ─────────────────────────────────────────────────────────────────────────────

static void print_dashboard(const MetricSnapshot& s) {
    std::print(
        "\n╔═══════════════════════════════════════════════════════════╗\n"
        "║  IO Metrics Dashboard  [snap #{}]\n"
        "╠═══════════════════════════════════════════════════════════╣\n"
        "║  Throughput : {:8.0f} ops/s  |  {:8.2f} MB/s\n"
        "║  Total events: {} ops  |  error rate {:.2f}%\n"
        "╠═════════════════ Latency Distribution ════════════════════╣\n"
        "║  p50   : {:6} µs  ({:5.2f} ms)\n"
        "║  p95   : {:6} µs  ({:5.2f} ms)\n"
        "║  p99   : {:6} µs  ({:5.2f} ms)\n"
        "║  p99.9 : {:6} µs  ({:5.2f} ms)\n"
        "║  max   : {:6} µs  ({:5.2f} ms)\n"
        "╠═════════════════ Event Type Distribution ══════════════════╣\n",
        s.snapshot_id,
        s.ops_per_sec, s.bytes_per_sec / (1024.0*1024.0),
        s.total_ops, s.error_rate_pct,
        s.lat_p50,  s.lat_p50  / 1000.0,
        s.lat_p95,  s.lat_p95  / 1000.0,
        s.lat_p99,  s.lat_p99  / 1000.0,
        s.lat_p999, s.lat_p999 / 1000.0,
        s.lat_max,  s.lat_max  / 1000.0);

    uint64_t total_ops = std::max(s.total_ops, uint64_t{1});
    for (int i = 0; i < static_cast<int>(IOEventType::_Count); ++i) {
        double pct = 100.0 * s.type_count[i] / total_ops;
        int bar = static_cast<int>(pct / 5.0);  // 5% per '#'
        std::print("║  {} : {:6}  ({:5.1f}%)  ",
                    event_name(static_cast<IOEventType>(i)),
                    s.type_count[i], pct);
        for (int b = 0; b < bar; ++b) std::print("{}", '#');
        std::println("");
    }
    std::println("╚═══════════════════════════════════════════════════════════╝");
}

// ─────────────────────────────────────────────────────────────────────────────
// §7  Main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::println("=== High-speed IO real-time data metrics dashboard ===");
    std::println("    4 producer threads × 20,000 events = 80,000 IO events");
    std::println("    StaticPipeline: classify → measure → aggregate → snap");
    std::println("    MessageBus: snapshot fan-out via 'metrics' topic\n");

    constexpr int kProducers       = 4;
    constexpr int kEventsPerThread = 20000;  // 80,000 events total

    Dispatcher disp(4);
    std::jthread worker([&] { disp.run(); });

    // ── §A  MessageBus setup — alert fan-out via "alerts" topic ──────────────
    MessageBus bus;
    bus.start(disp);

    // alert subscriber: print alert when p99 > 50ms or error rate > 5%
    auto sub_alert = bus.subscribe("alerts",
        [](MessageBus::Msg msg, Context) -> Task<Result<void>> {
            try {
                auto& snap = std::any_cast<MetricSnapshot&>(msg);
                if (snap.lat_p99 > 50000)
                    std::println("  [ALERT] p99 latency {:.1f}ms exceeded!",
                                snap.lat_p99 / 1000.0);
                if (snap.error_rate_pct > 5.0)
                    std::println("  [ALERT] error rate {:.2f}% threshold exceeded!",
                                snap.error_rate_pct);
            } catch (...) {}
            co_return Result<void>{};
        });

    // ── §B  StaticPipeline setup ─────────────────────────────────────────────
    std::println("── §A  IO metrics StaticPipeline setup ──\n");

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

    // ── §C  4 producer threads running concurrently ──────────────────────────
    std::println("── §B  Starting IO event generation ({} threads × {} events) ──\n",
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
    std::println("\n  [Production complete] {:.3f}s, {:.0f} ops/s (actual production throughput)",
                produce_sec,
                (kProducers * kEventsPerThread) / produce_sec);

    // wait for pipeline processing to complete
    std::this_thread::sleep_for(1s);

    disp.stop();
    worker.join();

    // ── §D  Final results summary ─────────────────────────────────────────────
    uint64_t final_ops    = g_stats.total_ops.load(std::memory_order_relaxed);
    uint64_t final_bytes  = g_stats.total_bytes.load(std::memory_order_relaxed);
    uint64_t final_errors = g_stats.error_count.load(std::memory_order_relaxed);
    // final summary: read via fill_bucket_counts without heap allocation
    constexpr size_t kSummaryBuckets = 16;
    uint64_t summary_buf[kSummaryBuckets]{};
    size_t summary_n = g_stats.latency_hist.fill_bucket_counts(summary_buf, kSummaryBuckets);
    uint64_t fp50{}, fp95{}, fp99{}, fp999{};
    compute_percentiles(summary_buf, summary_n, fp50, fp95, fp99, fp999);

    std::println("\n══════════════════════════════════════════════════════");
    std::println("  IO Metrics Final Summary");
    std::println("══════════════════════════════════════════════════════");
    std::println("  Total events  : {}", final_ops);
    std::println("  Total data    : {:.2f} MB", final_bytes / (1024.0*1024.0));
    std::println("  Error count   : {} ({:.2f}%)",
                final_errors,
                final_ops > 0 ? 100.0 * final_errors / final_ops : 0.0);
    std::println("  Snapshots recv: {}", g_snap_received.load());
    std::println("  p50 latency   : {} µs",  fp50);
    std::println("  p95 latency   : {} µs",  fp95);
    std::println("  p99 latency   : {} µs",  fp99);
    std::println("  p99.9 latency : {} µs",  fp999);
    std::println("  max latency   : {} µs",
                g_stats.lat_max.load(std::memory_order_relaxed));
    std::println("──────────────────────────────────────────────────────");

    // event type distribution
    std::println("  Event distribution:");
    for (int i = 0; i < static_cast<int>(IOEventType::_Count); ++i) {
        uint64_t c = g_stats.type_count[i].val.load(std::memory_order_relaxed);
        double   p = final_ops > 0 ? 100.0 * c / final_ops : 0.0;
        std::println("    {} {:6}  ({:.1f}%)",
                    event_name(static_cast<IOEventType>(i)),
                    c, p);
    }

    bool ok = (final_ops > 0 && g_snap_received.load() > 0);
    std::println("\nio_metrics_dashboard: {}", ok ? "ALL OK" : "WARN — no output");
    return 0;
}
