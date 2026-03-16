/**
 * @file tests/tracing_test.cpp
 * @brief Unit tests for TraceId, SpanId, TraceContext (W3C Trace Context).
 */

#include <gtest/gtest.h>
#include <qbuem/tracing/trace_context.hpp>

#include <set>
#include <string>

using namespace qbuem::tracing;

// ─── TraceId ──────────────────────────────────────────────────────────────────

TEST(TraceIdTest, GenerateProducesValidId) {
    auto id = TraceId::generate();
    EXPECT_TRUE(id.is_valid());
}

TEST(TraceIdTest, DefaultIsInvalid) {
    TraceId id{};
    EXPECT_FALSE(id.is_valid());
}

TEST(TraceIdTest, ToCharsProduces32HexChars) {
    auto id = TraceId::generate();
    char buf[64];
    size_t n = id.to_chars(buf, sizeof(buf));
    EXPECT_EQ(n, 32u);
    // All characters should be lowercase hex
    for (size_t i = 0; i < 32; ++i) {
        EXPECT_TRUE((buf[i] >= '0' && buf[i] <= '9') ||
                    (buf[i] >= 'a' && buf[i] <= 'f'))
            << "Non-hex character at position " << i << ": " << buf[i];
    }
}

TEST(TraceIdTest, ToCharsBufferTooSmall) {
    auto id = TraceId::generate();
    char buf[10];
    size_t n = id.to_chars(buf, sizeof(buf));
    EXPECT_EQ(n, 0u);
}

TEST(TraceIdTest, TwoGeneratedIdsAreDifferent) {
    auto id1 = TraceId::generate();
    auto id2 = TraceId::generate();
    // Extremely unlikely to be equal (128-bit random)
    bool same = true;
    for (int i = 0; i < 16; ++i) {
        if (id1.bytes[i] != id2.bytes[i]) { same = false; break; }
    }
    EXPECT_FALSE(same);
}

// ─── SpanId ───────────────────────────────────────────────────────────────────

TEST(SpanIdTest, GenerateProducesValidId) {
    auto id = SpanId::generate();
    EXPECT_TRUE(id.is_valid());
}

TEST(SpanIdTest, DefaultIsInvalid) {
    SpanId id{};
    EXPECT_FALSE(id.is_valid());
}

TEST(SpanIdTest, ToCharsProduces16HexChars) {
    auto id = SpanId::generate();
    char buf[32];
    size_t n = id.to_chars(buf, sizeof(buf));
    EXPECT_EQ(n, 16u);
    for (size_t i = 0; i < 16; ++i) {
        EXPECT_TRUE((buf[i] >= '0' && buf[i] <= '9') ||
                    (buf[i] >= 'a' && buf[i] <= 'f'))
            << "Non-hex char at " << i;
    }
}

TEST(SpanIdTest, ToCharsBufferTooSmall) {
    auto id = SpanId::generate();
    char buf[4];
    size_t n = id.to_chars(buf, sizeof(buf));
    EXPECT_EQ(n, 0u);
}

// ─── TraceContext ─────────────────────────────────────────────────────────────

TEST(TraceContextTest, GenerateProducesValidContext) {
    auto ctx = TraceContext::generate();
    EXPECT_TRUE(ctx.trace_id.is_valid());
    EXPECT_TRUE(ctx.parent_span_id.is_valid());
    EXPECT_EQ(ctx.flags, 1u);  // sampled by default
}

TEST(TraceContextTest, IsSampledWhenFlagBitSet) {
    TraceContext ctx = TraceContext::generate();
    ctx.flags = 0x01;
    EXPECT_TRUE(ctx.is_sampled());

    ctx.flags = 0x00;
    EXPECT_FALSE(ctx.is_sampled());
}

TEST(TraceContextTest, ToTraceparentLength) {
    auto ctx = TraceContext::generate();
    std::string tp = ctx.to_traceparent();
    EXPECT_EQ(tp.size(), 55u);
}

TEST(TraceContextTest, ToTraceparentFormat) {
    auto ctx = TraceContext::generate();
    std::string tp = ctx.to_traceparent();
    // "00-<32hex>-<16hex>-<2hex>"
    EXPECT_EQ(tp[0], '0');
    EXPECT_EQ(tp[1], '0');
    EXPECT_EQ(tp[2], '-');
    EXPECT_EQ(tp[35], '-');
    EXPECT_EQ(tp[52], '-');
}

TEST(TraceContextTest, FromTraceparentRoundTrip) {
    auto ctx = TraceContext::generate();
    std::string tp = ctx.to_traceparent();

    auto result = TraceContext::from_traceparent(tp);
    ASSERT_TRUE(result.has_value());

    for (int i = 0; i < 16; ++i)
        EXPECT_EQ(result->trace_id.bytes[i], ctx.trace_id.bytes[i]);
    for (int i = 0; i < 8; ++i)
        EXPECT_EQ(result->parent_span_id.bytes[i], ctx.parent_span_id.bytes[i]);
    EXPECT_EQ(result->flags, ctx.flags);
}

TEST(TraceContextTest, FromTraceparentInvalidTooShort) {
    auto result = TraceContext::from_traceparent("00-abc");
    EXPECT_FALSE(result.has_value());
}

TEST(TraceContextTest, FromTraceparentInvalidVersion) {
    // Version must be "00"
    std::string bad = "01-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
    auto result = TraceContext::from_traceparent(bad);
    EXPECT_FALSE(result.has_value());
}

TEST(TraceContextTest, ChildSpanSharesTraceId) {
    auto parent = TraceContext::generate();
    auto child  = parent.child_span();

    EXPECT_EQ(child.flags, parent.flags);
    for (int i = 0; i < 16; ++i)
        EXPECT_EQ(child.trace_id.bytes[i], parent.trace_id.bytes[i]);
}

TEST(TraceContextTest, ChildSpanHasNewSpanId) {
    auto parent = TraceContext::generate();
    auto child  = parent.child_span();

    bool same = true;
    for (int i = 0; i < 8; ++i) {
        if (child.parent_span_id.bytes[i] != parent.parent_span_id.bytes[i]) {
            same = false;
            break;
        }
    }
    EXPECT_FALSE(same);
}
