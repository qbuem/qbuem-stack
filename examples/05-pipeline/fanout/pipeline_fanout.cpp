/**
 * @file examples/pipeline_fanout.cpp
 * @brief Pipeline Guide §5-1: Fan-out (Broadcast) & Fan-in (Merge) 패턴 예시.
 *
 * 시나리오:
 *   RawLog 메시지가 들어오면 두 갈래로 팬아웃됩니다.
 *   - "main"  브랜치: 로그를 정규화해 메인 스토리지에 씁니다.
 *   - "audit" 브랜치: 감사(audit) 레코드를 별도로 기록합니다.
 *   두 결과는 PipelineGraph 출력 채널에서 팬인됩니다.
 *
 * 가이드 원문 (§5-1):
 *   Fan-out (Broadcast): Splitting one flow into multiple downstream paths.
 *   Fan-in  (Merge):     Collecting output from multiple sources into one sink.
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/pipeline_graph.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using namespace qbuem;
using namespace std::chrono_literals;

// ─── 도메인 타입 ─────────────────────────────────────────────────────────────

struct LogEntry {
    std::string raw;      ///< 원본 로그 문자열
    std::string branch;   ///< 처리 경로 레이블 ("main" | "audit")
    bool stored = false;
};

// ─── 액션 함수 ───────────────────────────────────────────────────────────────

// 소스 노드: 원본 문자열 그대로 전달
static Task<Result<LogEntry>> ingest(LogEntry entry) {
    co_return entry;
}

// 메인 브랜치: 정규화
static Task<Result<LogEntry>> normalize(LogEntry entry) {
    entry.branch = "main";
    entry.stored = true;
    co_return entry;
}

// 감사 브랜치: 감사 레코드 생성
static Task<Result<LogEntry>> audit(LogEntry entry) {
    entry.branch = "audit";
    entry.stored = true;
    co_return entry;
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    // PipelineGraph<LogEntry> 구성
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
        // Fan-in: 두 브랜치 모두 싱크
        .source("ingest")
        .sink("normalize")
        .sink("audit");

    Dispatcher dispatcher(2);
    auto output = graph.output();

    graph.start(dispatcher);
    std::jthread run_th([&] { dispatcher.run(); });

    // 메시지 투입
    constexpr size_t kItems = 5;
    for (size_t i = 0; i < kItems; ++i) {
        graph.try_push(LogEntry{"log-line-" + std::to_string(i)});
    }

    // 팬아웃이므로 각 입력 아이템이 두 브랜치에 복사 → 총 kItems*2 출력
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

    std::cout << "[fan-out] main="  << main_count.load()
              << " audit=" << audit_count.load() << "\n";

    return (main_count == kItems && audit_count == kItems) ? 0 : 1;
}
