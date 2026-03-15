/**
 * @file coroutine_context_test.cpp
 * @brief 코루틴 co_await 경계에서 Context 슬롯 기반 TraceContext 전파 검증
 *
 * ⚠️ §27 경고: 코루틴에서 thread_local은 위험합니다.
 * co_await 전후로 스레드가 바뀔 수 있기 때문에 thread_local 값이 소실됩니다.
 *
 * 이 테스트는 thread_local 대신 Context 슬롯(`TraceContextSlot`)을 사용하여
 * co_await 경계를 넘어서도 TraceContext가 올바르게 전파됨을 증명합니다.
 */

#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/pipeline.hpp>
#include <qbuem/tracing/exporter.hpp>
#include <qbuem/tracing/trace_context.hpp>
#include <gtest/gtest.h>

#include <string>

using namespace qbuem;
using namespace qbuem::tracing;

// ---------------------------------------------------------------------------
// 헬퍼: 동기적으로 Task<T>를 실행하고 결과를 반환
// ---------------------------------------------------------------------------
template <typename T>
T run_task(Task<T> task) {
    task.handle.resume();
    return std::move(*task.handle.promise().value);
}

void run_task_void(Task<> task) {
    task.handle.resume();
}

// ---------------------------------------------------------------------------
// 테스트 1: co_await 전후 Context 슬롯 보존
// ---------------------------------------------------------------------------

// co_await 가능한 즉시 재개 awaitable
struct ImmediateAwaiter {
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) noexcept { h.resume(); }
    void await_resume() const noexcept {}
};

Task<std::string> trace_id_across_await(Context ctx) {
    // co_await 이전에 TraceContextSlot 읽기
    auto before = ctx.get<TraceContextSlot>();
    EXPECT_TRUE(before.has_value());
    char buf[33]{}; before->value.trace_id.to_chars(buf, sizeof(buf));
    std::string id_before(buf);

    // co_await 경계를 넘음 (스레드 교체 가능)
    co_await ImmediateAwaiter{};

    // co_await 이후에도 ctx (값 타입)에서 TraceContextSlot 읽기
    auto after = ctx.get<TraceContextSlot>();
    EXPECT_TRUE(after.has_value());
    char buf2[33]{}; after->value.trace_id.to_chars(buf2, sizeof(buf2));
    std::string id_after(buf2);

    // 두 ID가 동일해야 함 (Context는 불변 값 타입이므로)
    EXPECT_EQ(id_before, id_after);
    co_return id_after;
}

TEST(CoroutineContext, TraceContextPreservedAcrossAwait) {
    // TraceContext 생성
    auto trace_ctx = TraceContext::generate();
    ASSERT_TRUE(trace_ctx.trace_id.is_valid());

    // Context 슬롯에 저장
    Context ctx;
    ctx = ctx.put(TraceContextSlot{trace_ctx});

    // 코루틴 실행
    auto result = run_task(trace_id_across_await(std::move(ctx)));

    // 결과 TraceId가 원본과 일치해야 함
    char expected[33]{}; trace_ctx.trace_id.to_chars(expected, sizeof(expected));
    EXPECT_EQ(result, std::string(expected));
}

// ---------------------------------------------------------------------------
// 테스트 2: child_span() 생성 후 Context 슬롯 업데이트
// ---------------------------------------------------------------------------

Task<bool> child_span_propagation(Context ctx) {
    auto slot = ctx.get<TraceContextSlot>();
    EXPECT_TRUE(slot.has_value()) << "부모 TraceContext 없음";

    // child span 생성 (같은 trace_id, 새 parent_span_id)
    auto child = slot->value.child_span();
    char child_tid[33]{}, parent_tid[33]{};
    child.trace_id.to_chars(child_tid, sizeof(child_tid));
    slot->value.trace_id.to_chars(parent_tid, sizeof(parent_tid));
    EXPECT_STREQ(child_tid, parent_tid);

    char child_sid[17]{}, parent_sid[17]{};
    child.parent_span_id.to_chars(child_sid, sizeof(child_sid));
    slot->value.parent_span_id.to_chars(parent_sid, sizeof(parent_sid));
    EXPECT_STRNE(child_sid, parent_sid);

    // ctx에 child span 저장
    ctx = ctx.put(TraceContextSlot{child});

    co_await ImmediateAwaiter{};

    // co_await 후 child span이 보존되어 있어야 함
    auto after = ctx.get<TraceContextSlot>();
    EXPECT_TRUE(after.has_value());
    char a_tid[33]{}, c_tid[33]{};
    after->value.trace_id.to_chars(a_tid, sizeof(a_tid));
    child.trace_id.to_chars(c_tid, sizeof(c_tid));
    EXPECT_STREQ(a_tid, c_tid);

    char a_sid[17]{}, c_sid[17]{};
    after->value.parent_span_id.to_chars(a_sid, sizeof(a_sid));
    child.parent_span_id.to_chars(c_sid, sizeof(c_sid));
    EXPECT_STREQ(a_sid, c_sid);

    co_return true;
}

TEST(CoroutineContext, ChildSpanPreservedAcrossAwait) {
    auto root = TraceContext::generate();
    Context ctx;
    ctx = ctx.put(TraceContextSlot{root});

    bool ok = run_task(child_span_propagation(std::move(ctx)));
    EXPECT_TRUE(ok);
}

// ---------------------------------------------------------------------------
// 테스트 3: 중첩 co_await — 여러 번 yield 후에도 Context 보존
// ---------------------------------------------------------------------------

Task<int> nested_awaits(Context ctx, int depth) {
    if (depth == 0) {
        auto slot = ctx.get<TraceContextSlot>();
        EXPECT_TRUE(slot.has_value());
        co_return 0;
    }
    co_await ImmediateAwaiter{};
    co_await ImmediateAwaiter{};

    // 재귀적 호출 (ctx를 값으로 전달)
    auto sub = nested_awaits(ctx, depth - 1);
    sub.handle.resume();
    int val = *sub.handle.promise().value;

    auto slot = ctx.get<TraceContextSlot>();
    EXPECT_TRUE(slot.has_value()) << "depth=" << depth << "에서 Context 유실";

    co_return val + 1;
}

TEST(CoroutineContext, ContextPreservedThroughMultipleAwaits) {
    auto trace_ctx = TraceContext::generate();
    Context ctx;
    ctx = ctx.put(TraceContextSlot{trace_ctx});

    int result = run_task(nested_awaits(ctx, 5));
    EXPECT_EQ(result, 5);
}

// ---------------------------------------------------------------------------
// 테스트 4: thread_local 위험 시연 (문서화 목적)
// ⚠️ 이 테스트는 thread_local이 단일 스레드에서는 유지됨을 보여주지만
//    멀티스레드 스케줄러에서는 소실될 수 있음을 경고합니다.
// ---------------------------------------------------------------------------

thread_local std::string tl_trace_id_UNSAFE;

Task<std::string> thread_local_danger_demo() {
    // thread_local 설정
    tl_trace_id_UNSAFE = "unsafe-trace-id";

    // co_await: 단일 스레드 환경에서는 유지되지만
    // 멀티스레드 Dispatcher에서는 다른 스레드에서 재개되면 소실됨!
    co_await ImmediateAwaiter{};

    // 단일 스레드 테스트에서는 통과하지만 이것은 안전하지 않습니다.
    co_return tl_trace_id_UNSAFE;
}

TEST(CoroutineContext, ThreadLocalDangerDocumented) {
    // ⚠️ 단일 스레드에서는 통과하지만 실제 Dispatcher에서는 위험!
    // 이 테스트는 단지 동작을 문서화하기 위한 것입니다.
    // 실제 코드에서는 반드시 Context 슬롯을 사용하세요.
    auto result = run_task(thread_local_danger_demo());
    // 단일 스레드에서는 유지됨
    EXPECT_EQ(result, "unsafe-trace-id");

    // 권장: thread_local 대신 Context 슬롯 사용
    // ctx.put(TraceContextSlot{...}) → ctx.get<TraceContextSlot>()
}
