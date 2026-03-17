/**
 * @file saga_example.cpp
 * @brief SagaOrchestrator — 분산 트랜잭션 (이커머스 주문 처리) 복합 예시.
 *
 * ## 시나리오: 주문 처리 사가
 * 고객이 상품을 구매할 때 여러 서비스에 걸친 트랜잭션이 필요합니다.
 *
 * 단계 1 [재고 예약]:
 *   execute:    상품 재고에서 수량 차감
 *   compensate: 재고 복원 (주문 취소 시)
 *
 * 단계 2 [결제 처리]:
 *   execute:    신용카드 결제 승인
 *   compensate: 결제 취소 / 환불
 *
 * 단계 3 [배송 요청]:
 *   execute:    배송 시스템에 출고 요청
 *   compensate: 출고 취소 요청
 *
 * ## 실패 시나리오
 * - 결제 실패 → 재고 복원 보상 트랜잭션 자동 실행
 * - 배송 실패 → 결제 환불 + 재고 복원 역순 보상 실행
 *
 * ## 커버리지
 * - SagaStep<T, T>: name / execute λ / compensate λ
 * - SagaOrchestrator<T>: add_step / run (성공/실패 경로)
 * - compensation_failures: 보상 실패 목록 확인
 */

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/context.hpp>
#include <qbuem/pipeline/saga.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;

// ─── 도메인 타입 ─────────────────────────────────────────────────────────────

struct OrderContext {
    uint64_t    order_id;
    uint64_t    product_id;
    int         quantity;
    double      total_krw;
    std::string card_token;

    // 실행 결과 상태 (단계별 채워짐)
    bool stock_reserved  = false;
    bool payment_charged = false;
    bool shipment_queued = false;
    std::string shipment_tracking_id;
    std::string pg_txn_id;
};

// ─── 서비스 상태 시뮬레이션 ──────────────────────────────────────────────────

static std::atomic<int> g_stock{10};              // 재고 수량
static std::atomic<int> g_payment_fail_mode{0};  // 0=정상 1=실패
static std::atomic<int> g_shipping_fail_mode{0};

// 보상 실행 추적
static std::vector<std::string> g_compensations;

// ─── 단계별 실행/보상 함수 ───────────────────────────────────────────────────

static Task<Result<OrderContext>> step_reserve_stock(OrderContext ord) {
    if (g_stock.load() < ord.quantity) {
        std::printf("  [재고예약] 재고 부족 (재고=%d, 요청=%d)\n",
                    g_stock.load(), ord.quantity);
        co_return unexpected(std::make_error_code(std::errc::no_space_on_device));
    }
    g_stock.fetch_sub(ord.quantity);
    ord.stock_reserved = true;
    std::printf("  [재고예약] 완료 — 잔여재고=%d\n", g_stock.load());
    co_return ord;
}

static Task<void> compensate_reserve_stock(OrderContext ord) {
    g_stock.fetch_add(ord.quantity);
    g_compensations.push_back("재고복원");
    std::printf("  [보상-재고복원] 재고 +%d 복원 → 잔여=%d\n",
                ord.quantity, g_stock.load());
    co_return;
}

static Task<Result<OrderContext>> step_charge_payment(OrderContext ord) {
    if (g_payment_fail_mode.load()) {
        std::puts("  [결제처리] 결제 거부 (시뮬레이션)");
        co_return unexpected(std::make_error_code(std::errc::permission_denied));
    }
    ord.payment_charged = true;
    ord.pg_txn_id = "PG-" + std::to_string(ord.order_id);
    std::printf("  [결제처리] 완료 — txn=%s 금액=%.0f원\n",
                ord.pg_txn_id.c_str(), ord.total_krw);
    co_return ord;
}

static Task<void> compensate_charge_payment(OrderContext ord) {
    g_compensations.push_back("결제취소");
    std::printf("  [보상-결제취소] txn=%s 환불 처리\n", ord.pg_txn_id.c_str());
    co_return;
}

static Task<Result<OrderContext>> step_request_shipment(OrderContext ord) {
    if (g_shipping_fail_mode.load()) {
        std::puts("  [배송요청] 배송 시스템 오류 (시뮬레이션)");
        co_return unexpected(std::make_error_code(std::errc::connection_refused));
    }
    ord.shipment_queued     = true;
    ord.shipment_tracking_id = "SHIP-" + std::to_string(ord.order_id);
    std::printf("  [배송요청] 완료 — 운송장=%s\n", ord.shipment_tracking_id.c_str());
    co_return ord;
}

static Task<void> compensate_request_shipment(OrderContext ord) {
    g_compensations.push_back("출고취소");
    std::printf("  [보상-출고취소] 운송장=%s 취소\n", ord.shipment_tracking_id.c_str());
    co_return;
}

// ─── SagaOrchestrator 구성 ───────────────────────────────────────────────────

static SagaOrchestrator<OrderContext> build_order_saga() {
    SagaOrchestrator<OrderContext> saga;
    saga.add_step(SagaStep<OrderContext, OrderContext>{
        .name       = "재고예약",
        .execute    = step_reserve_stock,
        .compensate = compensate_reserve_stock,
    });
    saga.add_step(SagaStep<OrderContext, OrderContext>{
        .name       = "결제처리",
        .execute    = step_charge_payment,
        .compensate = compensate_charge_payment,
    });
    saga.add_step(SagaStep<OrderContext, OrderContext>{
        .name       = "배송요청",
        .execute    = step_request_shipment,
        .compensate = compensate_request_shipment,
    });
    return saga;
}

// ─── RunGuard ─────────────────────────────────────────────────────────────────

struct RunGuard {
    Dispatcher  dispatcher;
    std::jthread thread;
    explicit RunGuard(size_t n = 1) : dispatcher(n) {
        thread = std::jthread([this] { dispatcher.run(); });
    }
    ~RunGuard() { dispatcher.stop(); if (thread.joinable()) thread.join(); }
    template <typename F>
    void run_and_wait(F&& f, std::chrono::milliseconds timeout = 5s) {
        std::atomic<bool> done{false};
        dispatcher.spawn([&, f = std::forward<F>(f)]() mutable -> Task<void> {
            co_await f(); done.store(true);
        }());
        auto dl = std::chrono::steady_clock::now() + timeout;
        while (!done.load() && std::chrono::steady_clock::now() < dl)
            std::this_thread::sleep_for(1ms);
    }
};

// ─── 시나리오 1: 정상 주문 ───────────────────────────────────────────────────

static void scenario_happy_path() {
    std::puts("\n=== 시나리오 1: 정상 주문 ===");
    g_stock.store(10);
    g_payment_fail_mode.store(0);
    g_shipping_fail_mode.store(0);
    g_compensations.clear();

    auto saga = build_order_saga();
    OrderContext ord{1001, 42, 2, 59800.0, "tok_visa_4242"};

    RunGuard guard;
    bool success = false;
    OrderContext result{};

    guard.run_and_wait([&]() -> Task<void> {
        auto res = co_await saga.run(ord, {});
        success  = res.has_value();
        if (success) result = *res;
    });

    std::printf("[결과] 성공=%s 재고예약=%s 결제=%s 배송=%s\n",
                success ? "YES" : "NO",
                result.stock_reserved  ? "OK" : "FAIL",
                result.payment_charged ? "OK" : "FAIL",
                result.shipment_queued ? "OK" : "FAIL");
    std::printf("[결과] 운송장=%s, 남은재고=%d\n",
                result.shipment_tracking_id.c_str(), g_stock.load());
    if (!g_compensations.empty()) std::puts("[보상] 예상치 않은 보상 실행!");
}

// ─── 시나리오 2: 결제 실패 → 재고 보상 ──────────────────────────────────────

static void scenario_payment_failure() {
    std::puts("\n=== 시나리오 2: 결제 실패 → 자동 재고 복원 ===");
    g_stock.store(10);
    g_payment_fail_mode.store(1);  // 결제 강제 실패
    g_shipping_fail_mode.store(0);
    g_compensations.clear();

    auto saga = build_order_saga();
    OrderContext ord{1002, 42, 3, 89700.0, "tok_mastercard_5555"};

    RunGuard guard;
    bool failed = false;

    guard.run_and_wait([&]() -> Task<void> {
        auto res = co_await saga.run(ord, {});
        failed   = !res.has_value();
        if (failed)
            std::printf("[결과] 사가 실패: %s\n", res.error().message().c_str());
    });

    std::printf("[보상] 실행된 보상: ");
    for (const auto& c : g_compensations) std::printf("%s ", c.c_str());
    std::printf("\n[보상] 재고 복원됨: %d (원래=10, 결제 실패 전 차감=3)\n",
                g_stock.load());
}

// ─── 시나리오 3: 배송 실패 → 결제 환불 + 재고 복원 ─────────────────────────

static void scenario_shipping_failure() {
    std::puts("\n=== 시나리오 3: 배송 실패 → 결제 환불 + 재고 복원 ===");
    g_stock.store(10);
    g_payment_fail_mode.store(0);
    g_shipping_fail_mode.store(1);  // 배송 강제 실패
    g_compensations.clear();

    auto saga = build_order_saga();
    OrderContext ord{1003, 99, 1, 150000.0, "tok_amex_3782"};

    RunGuard guard;
    bool failed = false;

    guard.run_and_wait([&]() -> Task<void> {
        auto res = co_await saga.run(ord, {});
        failed   = !res.has_value();
    });

    std::printf("[보상] 실행된 보상 (역순): ");
    for (const auto& c : g_compensations) std::printf("[%s] ", c.c_str());
    std::printf("\n[결과] 재고=%d (10 복원됨)\n", g_stock.load());
    // 보상 실패 확인
    const auto& cf = saga.compensation_failures();
    std::printf("[보상실패] %zu건\n", cf.size());
}

// ─── 시나리오 4: 재고 부족 ───────────────────────────────────────────────────

static void scenario_out_of_stock() {
    std::puts("\n=== 시나리오 4: 재고 부족 (첫 단계 실패) ===");
    g_stock.store(1);  // 재고 1개
    g_payment_fail_mode.store(0);
    g_shipping_fail_mode.store(0);
    g_compensations.clear();

    auto saga = build_order_saga();
    OrderContext ord{1004, 77, 5, 250000.0, "tok_visa_1234"};  // 5개 요청

    RunGuard guard;
    bool failed = false;

    guard.run_and_wait([&]() -> Task<void> {
        auto res = co_await saga.run(ord, {});
        failed   = !res.has_value();
        if (failed)
            std::printf("[결과] 사가 실패: %s\n", res.error().message().c_str());
    });

    // 첫 단계에서 실패 → 보상 없음 (아직 아무것도 실행되지 않음)
    std::printf("[보상] 실행 없음: %s\n", g_compensations.empty() ? "YES" : "NO");
    std::printf("[재고] 변화 없음=%d\n", g_stock.load());
}

int main() {
    scenario_happy_path();
    scenario_payment_failure();
    scenario_shipping_failure();
    scenario_out_of_stock();
    std::puts("\nsaga_example: ALL OK");
    return 0;
}
