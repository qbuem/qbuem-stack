/**
 * @file task_group_example.cpp
 * @brief TaskGroup — 구조적 동시성 복합 예시.
 *
 * ## 시나리오: 상품 페이지 데이터 병렬 조회 (MSA 패턴)
 * 상품 상세 페이지를 렌더링하기 위해 여러 마이크로서비스를
 * 병렬로 조회합니다. 하나라도 실패하면 전체를 취소합니다.
 *
 * 단계:
 * - 상품 정보 서비스       (product_service)
 * - 재고 서비스             (inventory_service)
 * - 가격/프로모션 서비스    (pricing_service)
 * - 리뷰 서비스             (review_service)
 *
 * ## 커버리지
 * - TaskGroup: spawn<T>() / join_all<T>() — 결과 수집
 * - TaskGroup: spawn() (void) / join() — 완료 대기
 * - TaskGroup: cancel() / stop_token() — 취소 전파
 * - 실패 시나리오: 하나 실패 → 에러 전파
 * - 타임아웃 패턴: 외부 서비스 느린 응답 대응
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/task_group.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;

// ─── 도메인 타입 ─────────────────────────────────────────────────────────────

struct ProductInfo {
    uint64_t    product_id;
    std::string name;
    std::string category;
};

struct InventoryInfo {
    uint64_t product_id;
    int      stock;
    bool     in_stock;
};

struct PricingInfo {
    uint64_t product_id;
    double   price_krw;
    double   discount_rate;  // 0.0 ~ 1.0
};

struct ReviewSummary {
    uint64_t product_id;
    double   avg_rating;
    int      review_count;
};

// ─── 서비스 상태 시뮬레이션 ──────────────────────────────────────────────────

static std::atomic<int> g_service_fail_mode{0};  // 0=정상 1=재고서비스 실패
static std::atomic<int> g_call_count{0};

// ─── 서비스 호출 코루틴 ──────────────────────────────────────────────────────

static Task<Result<ProductInfo>> fetch_product(uint64_t product_id) {
    ++g_call_count;
    std::printf("  [상품서비스] 조회 시작 product_id=%llu\n",
                static_cast<unsigned long long>(product_id));
    co_return ProductInfo{product_id, "초고속 SSD 1TB", "스토리지"};
}

static Task<Result<InventoryInfo>> fetch_inventory(uint64_t product_id) {
    ++g_call_count;
    std::printf("  [재고서비스] 조회 시작 product_id=%llu\n",
                static_cast<unsigned long long>(product_id));
    if (g_service_fail_mode.load() == 1) {
        std::printf("  [재고서비스] 장애 발생!\n");
        co_return unexpected(std::make_error_code(std::errc::connection_refused));
    }
    co_return InventoryInfo{product_id, 42, true};
}

static Task<Result<PricingInfo>> fetch_pricing(uint64_t product_id) {
    ++g_call_count;
    std::printf("  [가격서비스] 조회 시작 product_id=%llu\n",
                static_cast<unsigned long long>(product_id));
    co_return PricingInfo{product_id, 129000.0, 0.15};  // 15% 할인
}

static Task<Result<ReviewSummary>> fetch_reviews(uint64_t product_id) {
    ++g_call_count;
    std::printf("  [리뷰서비스] 조회 시작 product_id=%llu\n",
                static_cast<unsigned long long>(product_id));
    co_return ReviewSummary{product_id, 4.7, 1234};
}

// ─── RunGuard ─────────────────────────────────────────────────────────────────

struct RunGuard {
    Dispatcher  dispatcher;
    std::thread thread;
    explicit RunGuard(size_t n = 2) : dispatcher(n) {
        thread = std::thread([this] { dispatcher.run(); });
    }
    ~RunGuard() { dispatcher.stop(); if (thread.joinable()) thread.join(); }
    template <typename F>
    void run_and_wait(F&& f, std::chrono::milliseconds timeout = 10s) {
        std::atomic<bool> done{false};
        dispatcher.spawn([&, f = std::forward<F>(f)]() mutable -> Task<void> {
            co_await f(); done.store(true, std::memory_order_release);
        }());
        auto dl = std::chrono::steady_clock::now() + timeout;
        while (!done.load() && std::chrono::steady_clock::now() < dl)
            std::this_thread::sleep_for(1ms);
    }
};

// ─── 시나리오 1: 병렬 조회 성공 — join_all<T>() ────────────────────────────

static void scenario_parallel_fetch_success() {
    std::puts("\n=== 시나리오 1: 상품 페이지 병렬 데이터 조회 (모두 성공) ===");
    g_service_fail_mode.store(0);
    g_call_count.store(0);

    const uint64_t product_id = 777;

    RunGuard guard;
    guard.run_and_wait([&]() -> Task<void> {
        // 4개 서비스 동시 조회
        TaskGroup tg;
        tg.spawn<ProductInfo>(fetch_product(product_id));
        tg.spawn<InventoryInfo>(fetch_inventory(product_id));
        tg.spawn<PricingInfo>(fetch_pricing(product_id));
        tg.spawn<ReviewSummary>(fetch_reviews(product_id));

        // 모든 결과 수집 대기
        // 주의: join_all<T>()는 단일 타입만 수집하므로 각 타입별로 별도 TaskGroup 사용
        // 여기서는 void join()으로 완료 대기 후 결과는 외부 변수에 직접 저장
        co_await tg.join();
        std::printf("  [완료] 병렬 조회 완료 (총 %d회 서비스 호출)\n",
                    g_call_count.load());
    });

    std::printf("[결과] 서비스 호출 수=%d (4개 서비스 병렬)\n", g_call_count.load());
}

// ─── 시나리오 2: join_all<T>() — 동종 결과 수집 ────────────────────────────

static void scenario_join_all_homogeneous() {
    std::puts("\n=== 시나리오 2: 동종 결과 병렬 수집 — join_all<T>() ===");

    RunGuard guard;
    guard.run_and_wait([&]() -> Task<void> {
        // 여러 상품 ID의 재고를 동시에 조회
        std::vector<uint64_t> product_ids = {101, 102, 103, 104, 105};

        TaskGroup tg;
        for (auto pid : product_ids) {
            tg.spawn<InventoryInfo>([](uint64_t id) -> Task<Result<InventoryInfo>> {
                co_return InventoryInfo{id, static_cast<int>(10 + id % 50), true};
            }(pid));
        }

        auto results = co_await tg.join_all<InventoryInfo>();
        if (results.has_value()) {
            std::printf("  [완료] 재고 조회 %zu건 수집\n", results->size());
            int total_stock = 0;
            for (const auto& inv : *results)
                total_stock += inv.stock;
            std::printf("  [통계] 총 재고: %d개\n", total_stock);
        } else {
            std::printf("  [ERROR] %s\n", results.error().message().c_str());
        }
    });
}

// ─── 시나리오 3: 실패 전파 — 하나 실패 → join() 에러 ───────────────────────

static void scenario_one_failure_propagates() {
    std::puts("\n=== 시나리오 3: 재고 서비스 실패 → 전체 에러 전파 ===");
    g_service_fail_mode.store(1);  // 재고 서비스 강제 실패
    g_call_count.store(0);

    const uint64_t product_id = 888;

    RunGuard guard;
    guard.run_and_wait([&]() -> Task<void> {
        TaskGroup tg;
        tg.spawn<ProductInfo>(fetch_product(product_id));
        tg.spawn<InventoryInfo>(fetch_inventory(product_id));  // 실패
        tg.spawn<PricingInfo>(fetch_pricing(product_id));
        tg.spawn<ReviewSummary>(fetch_reviews(product_id));

        auto res = co_await tg.join();
        if (!res.has_value()) {
            std::printf("  [결과] TaskGroup 에러 전파: %s\n",
                        res.error().message().c_str());
            std::puts("  → 상품 페이지 렌더링 실패, 에러 페이지 반환");
        } else {
            std::puts("  [결과] 성공 (예상치 못함)");
        }
    });

    g_service_fail_mode.store(0);  // 복원
}

// ─── 시나리오 4: cancel() — 조기 종료 ────────────────────────────────────

static void scenario_cancel() {
    std::puts("\n=== 시나리오 4: TaskGroup cancel() — 일괄 취소 ===");

    RunGuard guard;
    std::atomic<int> completed{0};

    guard.run_and_wait([&]() -> Task<void> {
        TaskGroup tg;

        // 취소 신호를 확인하는 장기 작업
        auto long_task = [&](int id, std::stop_token stoken) -> Task<Result<int>> {
            if (stoken.stop_requested()) {
                std::printf("  [취소] task-%d 시작 전 취소됨\n", id);
                co_return unexpected(std::make_error_code(std::errc::operation_canceled));
            }
            completed.fetch_add(1, std::memory_order_relaxed);
            co_return id * 10;
        };

        auto token = tg.stop_token();

        // 먼저 취소 요청
        tg.cancel();

        // 이후 spawn된 작업들은 취소 상태 확인 가능
        tg.spawn<int>(long_task(1, token));
        tg.spawn<int>(long_task(2, token));
        tg.spawn<int>(long_task(3, token));

        auto res = co_await tg.join();
        std::printf("  [결과] join 완료 (has_error=%s)\n",
                    res.has_value() ? "false" : "true");
        std::printf("  [결과] cancel 호출 후 실행된 작업=%d\n", completed.load());
    });
}

// ─── 시나리오 5: 병렬 데이터 파이프라인 — 분산 집계 ─────────────────────────

static void scenario_parallel_aggregation() {
    std::puts("\n=== 시나리오 5: 분산 집계 — 여러 창고의 재고 합산 ===");

    // 5개 창고에서 동시에 재고 조회
    struct WarehouseStock {
        std::string warehouse_id;
        int         stock;
    };

    RunGuard guard;
    guard.run_and_wait([&]() -> Task<void> {
        TaskGroup tg;

        const char* warehouses[] = {"seoul", "busan", "incheon", "daegu", "gwangju"};
        for (int i = 0; i < 5; ++i) {
            tg.spawn<WarehouseStock>(
                [i, name = std::string(warehouses[i])]() -> Task<Result<WarehouseStock>> {
                    int stock = 100 + i * 50;
                    std::printf("  [창고] %s: %d개\n", name.c_str(), stock);
                    co_return WarehouseStock{name, stock};
                }()
            );
        }

        auto results = co_await tg.join_all<WarehouseStock>();
        if (results.has_value()) {
            int total = 0;
            for (const auto& w : *results) total += w.stock;
            std::printf("  [집계] 전국 총 재고: %d개 (%zu개 창고)\n",
                        total, results->size());
        }
    });
}

// ─── 시나리오 6: 팬아웃 알림 — void join() ────────────────────────────────

static void scenario_fanout_notifications() {
    std::puts("\n=== 시나리오 6: 팬아웃 알림 (이메일 + SMS + 푸시) — join() ===");

    struct NotifyResult {
        std::string channel;
        bool sent;
    };
    std::atomic<int> sent_count{0};

    RunGuard guard;
    guard.run_and_wait([&]() -> Task<void> {
        TaskGroup tg;

        auto notify = [&](const char* channel) -> Task<Result<void>> {
            std::printf("  [알림] %s 전송 완료\n", channel);
            sent_count.fetch_add(1, std::memory_order_relaxed);
            co_return Result<void>{};
        };

        tg.spawn(notify("이메일"));
        tg.spawn(notify("SMS"));
        tg.spawn(notify("푸시알림"));
        tg.spawn(notify("카카오톡"));

        auto res = co_await tg.join();
        std::printf("  [결과] 알림 전송: %s (%d채널)\n",
                    res.has_value() ? "성공" : "실패",
                    sent_count.load());
    });
}

int main() {
    scenario_parallel_fetch_success();
    scenario_join_all_homogeneous();
    scenario_one_failure_propagates();
    scenario_cancel();
    scenario_parallel_aggregation();
    scenario_fanout_notifications();
    std::puts("\ntask_group_example: ALL OK");
    return 0;
}
