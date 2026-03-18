/**
 * @file coroutine_context_test.cpp
 * @brief Verifies slot-based TraceContext propagation across co_await boundaries.
 *
 * ⚠️ §27 Warning: thread_local is dangerous inside coroutines.
 * The executing thread can change across co_await, causing thread_local values
 * to be lost on the resuming thread.
 *
 * This test uses Context slots (TraceContextSlot) instead of thread_local to
 * prove that TraceContext is correctly propagated across co_await boundaries.
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
// Helper: run a Task<T> synchronously and return its result
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
// Test 1: Context slot preserved across co_await boundary
// ---------------------------------------------------------------------------

// Awaitable that suspends and immediately resumes
struct ImmediateAwaiter {
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) noexcept { h.resume(); }
    void await_resume() const noexcept {}
};

Task<std::string> trace_id_across_await(Context ctx) {
    // Read TraceContextSlot before co_await
    auto before = ctx.get<TraceContextSlot>();
    EXPECT_TRUE(before.has_value());
    char buf[33]{}; before->value.trace_id.to_chars(buf, sizeof(buf));
    std::string id_before(buf);

    // Cross the co_await boundary (thread may change in multi-threaded dispatcher)
    co_await ImmediateAwaiter{};

    // Read TraceContextSlot after co_await from the (value-type) ctx
    auto after = ctx.get<TraceContextSlot>();
    EXPECT_TRUE(after.has_value());
    char buf2[33]{}; after->value.trace_id.to_chars(buf2, sizeof(buf2));
    std::string id_after(buf2);

    // Both IDs must be equal (Context is an immutable value type)
    EXPECT_EQ(id_before, id_after);
    co_return id_after;
}

TEST(CoroutineContext, TraceContextPreservedAcrossAwait) {
    // Generate a TraceContext
    auto trace_ctx = TraceContext::generate();
    ASSERT_TRUE(trace_ctx.trace_id.is_valid());

    // Store in a Context slot
    Context ctx;
    ctx = ctx.put(TraceContextSlot{trace_ctx});

    // Run the coroutine
    auto result = run_task(trace_id_across_await(std::move(ctx)));

    // The result TraceId must match the original
    char expected[33]{}; trace_ctx.trace_id.to_chars(expected, sizeof(expected));
    EXPECT_EQ(result, std::string(expected));
}

// ---------------------------------------------------------------------------
// Test 2: Context slot updated after child_span() creation
// ---------------------------------------------------------------------------

Task<bool> child_span_propagation(Context ctx) {
    auto slot = ctx.get<TraceContextSlot>();
    EXPECT_TRUE(slot.has_value()) << "parent TraceContext not found";

    // Create a child span (same trace_id, new parent_span_id)
    auto child = slot->value.child_span();
    char child_tid[33]{}, parent_tid[33]{};
    child.trace_id.to_chars(child_tid, sizeof(child_tid));
    slot->value.trace_id.to_chars(parent_tid, sizeof(parent_tid));
    EXPECT_STREQ(child_tid, parent_tid);

    char child_sid[17]{}, parent_sid[17]{};
    child.parent_span_id.to_chars(child_sid, sizeof(child_sid));
    slot->value.parent_span_id.to_chars(parent_sid, sizeof(parent_sid));
    EXPECT_STRNE(child_sid, parent_sid);

    // Store the child span in ctx
    ctx = ctx.put(TraceContextSlot{child});

    co_await ImmediateAwaiter{};

    // Child span must be preserved after co_await
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
// Test 3: Nested co_awaits — Context preserved through multiple yields
// ---------------------------------------------------------------------------

Task<int> nested_awaits(Context ctx, int depth) {
    if (depth == 0) {
        auto slot = ctx.get<TraceContextSlot>();
        EXPECT_TRUE(slot.has_value());
        co_return 0;
    }
    co_await ImmediateAwaiter{};
    co_await ImmediateAwaiter{};

    // Recursive call (ctx passed by value)
    auto sub = nested_awaits(ctx, depth - 1);
    sub.handle.resume();
    int val = *sub.handle.promise().value;

    auto slot = ctx.get<TraceContextSlot>();
    EXPECT_TRUE(slot.has_value()) << "Context lost at depth=" << depth;

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
// Test 4: thread_local danger demo (documentation purpose)
// ⚠️ This test shows that thread_local persists on a single-threaded scheduler
//    but warns it will be lost in a multi-threaded dispatcher.
// ---------------------------------------------------------------------------

thread_local std::string tl_trace_id_UNSAFE;

Task<std::string> thread_local_danger_demo() {
    // Set a thread_local value
    tl_trace_id_UNSAFE = "unsafe-trace-id";

    // co_await: safe on a single thread, but on a multi-threaded Dispatcher
    // the coroutine may resume on a different thread, losing the value!
    co_await ImmediateAwaiter{};

    // Passes in single-threaded tests, but this is NOT safe in production.
    co_return tl_trace_id_UNSAFE;
}

TEST(CoroutineContext, ThreadLocalDangerDocumented) {
    // ⚠️ Passes on a single thread but dangerous with a real multi-threaded Dispatcher!
    // This test exists only to document the behavior.
    // In real code, always use Context slots instead of thread_local.
    auto result = run_task(thread_local_danger_demo());
    // Persists in single-threaded execution
    EXPECT_EQ(result, "unsafe-trace-id");

    // Recommended pattern: use Context slots instead of thread_local
    // ctx.put(TraceContextSlot{...}) → ctx.get<TraceContextSlot>()
}
