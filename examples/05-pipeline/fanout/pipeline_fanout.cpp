/**
 * @file examples/pipeline_fanout.cpp
 * @brief Pipeline Guide §5-1: Fan-out (Broadcast) & Fan-in (Merge) pattern example.
 *
 * Scenario:
 *   Incoming RawLog messages are fanned out to two branches.
 *   - "main"  branch: normalizes the log and writes to main storage.
 *   - "audit" branch: records a separate audit entry.
 *   Both results are fanned in at the PipelineGraph output channel.
 *
 * Guide excerpt (§5-1):
 *   Fan-out (Broadcast): Splitting one flow into multiple downstream paths.
 *   Fan-in  (Merge):     Collecting output from multiple sources into one sink.
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/pipeline_graph.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <qbuem/compat/print.hpp>

using namespace qbuem;
using namespace std::chrono_literals;
using std::println;

// ─── Domain types ─────────────────────────────────────────────────────────────

struct LogEntry {
    std::string raw;      ///< raw log string
    std::string branch;   ///< processing path label ("main" | "audit")
    bool stored = false;
};

// ─── Action functions ─────────────────────────────────────────────────────────

// Source node: pass through the raw string unchanged
static Task<Result<LogEntry>> ingest(LogEntry entry) {
    co_return entry;
}

// Main branch: normalize
static Task<Result<LogEntry>> normalize(LogEntry entry) {
    entry.branch = "main";
    entry.stored = true;
    co_return entry;
}

// Audit branch: produce audit record
static Task<Result<LogEntry>> audit(LogEntry entry) {
    entry.branch = "audit";
    entry.stored = true;
    co_return entry;
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main() {
    // Build PipelineGraph<LogEntry>
    //
    //  [ingest] ──► [normalize]  ──► sink
    //           └─► [audit]      ──► sink
    //
    PipelineGraph<LogEntry> graph;
    graph
        .node("ingest",    ingest,    1, 64)
        .node("normalize", normalize, 2, 64)
        .node("audit",     audit,     1, 64)
        // Fan-out: ingest → normalize, ingest → audit
        .edge("ingest", "normalize")
        .edge("ingest", "audit")
        // Fan-in: both branches feed the sink
        .source("ingest")
        .sink("normalize")
        .sink("audit");

    Dispatcher dispatcher(2);
    auto output = graph.output();

    graph.start(dispatcher);
    std::jthread run_th([&] { dispatcher.run(); });

    // Push messages
    constexpr size_t kItems = 5;
    for (size_t i = 0; i < kItems; ++i) {
        graph.try_push(LogEntry{"log-line-" + std::to_string(i)});
    }

    // Fan-out: each input item is copied to both branches → total kItems*2 outputs
    std::atomic<size_t> main_count{0}, audit_count{0};
    auto deadline = std::chrono::steady_clock::now() + 5s;

    while ((main_count + audit_count) < kItems * 2 &&
           std::chrono::steady_clock::now() < deadline) {
        auto item = output->try_recv();
        if (item) {
            if (item->value.branch == "main")  ++main_count;
            else                               ++audit_count;
        } else {
            std::this_thread::sleep_for(1ms);
        }
    }

    dispatcher.stop();
    run_th.join();

    println("[fan-out] main={} audit={}", main_count.load(), audit_count.load());

    return (main_count == kItems && audit_count == kItems) ? 0 : 1;
}
