// config_tracing_coverage_test.cpp
//
// Coverage tests for:
//   - include/qbuem/config/config_manager.hpp
//       Secret<T>, ConfigValue, ConfigTable<Cap>, ConfigManager
//   - include/qbuem/tracing/trace_context.hpp
//       TraceId, SpanId, TraceContext (W3C traceparent parse/format/roundtrip)
//   - include/qbuem/tracing/span.hpp        (SpanData, Span RAII)
//   - include/qbuem/tracing/sampler.hpp     (all built-in samplers)
//   - include/qbuem/tracing/exporter.hpp    (NoopSpanExporter, LoggingSpanExporter,
//                                            Tracer, PipelineTracer, PrometheusTextExporter)
//   - include/qbuem/tracing/trace_logger.hpp (TraceLogRecord, TraceLogRing, level_str)
//   - include/qbuem/tracing/lifecycle_tracer.hpp (SpanRecord, ShmSpanRing,
//                                                  ActiveSpan, LifecycleTracer)
//
// All errors are std::expected (Result<T>) — value and error paths are tested.
// Deterministic, single-process, no real network / no servers.

#include <gtest/gtest.h>

#include <qbuem/config/config_manager.hpp>
#include <qbuem/tracing/trace_context.hpp>
#include <qbuem/tracing/span.hpp>
#include <qbuem/tracing/sampler.hpp>
#include <qbuem/tracing/exporter.hpp>
#include <qbuem/tracing/trace_logger.hpp>
#include <qbuem/tracing/lifecycle_tracer.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cfg = qbuem::config;
namespace tr  = qbuem::tracing;

// ============================================================================
// Secret<T>
// ============================================================================

TEST(Secret, DefaultIsEmpty) {
    cfg::Secret<std::string> s;
    EXPECT_FALSE(s.has_value());
    EXPECT_TRUE(s.reveal().empty());
}

TEST(Secret, RevealReturnsValue) {
    cfg::Secret<std::string> s{std::string{"s3cr3t-api-key"}};
    EXPECT_TRUE(s.has_value());
    EXPECT_EQ(s.reveal(), "s3cr3t-api-key");
}

TEST(Secret, FormatAlwaysRedacted) {
    cfg::Secret<std::string> s{std::string{"super-secret"}};
    std::string out = std::format("{}", s);
    EXPECT_EQ(out, "[REDACTED]");
    // Plaintext must never leak via formatting.
    EXPECT_EQ(out.find("super-secret"), std::string::npos);
}

TEST(Secret, MoveTransfersOwnershipAndWipesSource) {
    cfg::Secret<std::string> a{std::string{"abc123"}};
    cfg::Secret<std::string> b{std::move(a)};
    EXPECT_TRUE(b.has_value());
    EXPECT_EQ(b.reveal(), "abc123");
    // Source was wiped after move.
    EXPECT_FALSE(a.has_value());  // NOLINT(bugprone-use-after-move)
}

TEST(Secret, MoveAssignWipesSource) {
    cfg::Secret<std::string> a{std::string{"value-A"}};
    cfg::Secret<std::string> b{std::string{"value-B"}};
    b = std::move(a);
    EXPECT_EQ(b.reveal(), "value-A");
    EXPECT_FALSE(a.has_value());  // NOLINT(bugprone-use-after-move)
}

TEST(Secret, EmptyStringHasNoValue) {
    cfg::Secret<std::string> s{std::string{""}};
    EXPECT_FALSE(s.has_value());
}

// ============================================================================
// ConfigValue
// ============================================================================

TEST(ConfigValue, UnsetByDefault) {
    cfg::ConfigValue v;
    EXPECT_EQ(v.type(), cfg::ConfigValue::Type::Unset);
    EXPECT_FALSE(v.is_set());
    EXPECT_EQ(v.as_int(), 0);
    EXPECT_EQ(v.as_double(), 0.0);
    EXPECT_FALSE(v.as_bool());
    EXPECT_TRUE(v.as_string().empty());
}

TEST(ConfigValue, IntStoresAndReads) {
    cfg::ConfigValue v{int64_t{8080}};
    EXPECT_EQ(v.type(), cfg::ConfigValue::Type::Int);
    EXPECT_TRUE(v.is_set());
    EXPECT_EQ(v.as_int(), 8080);
    // Wrong-type access returns safe defaults.
    EXPECT_EQ(v.as_double(), 0.0);
    EXPECT_FALSE(v.as_bool());
    EXPECT_TRUE(v.as_string().empty());
}

TEST(ConfigValue, DoubleStoresAndReads) {
    cfg::ConfigValue v{3.14};
    EXPECT_EQ(v.type(), cfg::ConfigValue::Type::Double);
    EXPECT_DOUBLE_EQ(v.as_double(), 3.14);
    EXPECT_EQ(v.as_int(), 0);
}

TEST(ConfigValue, BoolStoresAndReads) {
    cfg::ConfigValue vt{true};
    cfg::ConfigValue vf{false};
    EXPECT_EQ(vt.type(), cfg::ConfigValue::Type::Bool);
    EXPECT_TRUE(vt.as_bool());
    EXPECT_FALSE(vf.as_bool());
}

TEST(ConfigValue, StringStoresAndReads) {
    cfg::ConfigValue v{std::string_view{"hello"}};
    EXPECT_EQ(v.type(), cfg::ConfigValue::Type::String);
    EXPECT_EQ(v.as_string(), "hello");
    EXPECT_EQ(v.as_int(), 0);
}

TEST(ConfigValue, StringTruncatedAtInlineMax) {
    // kInlineMax = 255, so a 300-char string is truncated to 255 chars.
    std::string big(300, 'x');
    cfg::ConfigValue v{std::string_view{big}};
    EXPECT_EQ(v.as_string().size(), 255u);
}

TEST(ConfigValue, EmptyStringIsSet) {
    cfg::ConfigValue v{std::string_view{""}};
    EXPECT_EQ(v.type(), cfg::ConfigValue::Type::String);
    EXPECT_TRUE(v.is_set());
    EXPECT_TRUE(v.as_string().empty());
}

// ============================================================================
// ConfigTable<Cap>
// ============================================================================

TEST(ConfigTable, InsertAndFind) {
    cfg::ConfigTable<8> t;
    EXPECT_TRUE(t.insert_or_assign(42, cfg::ConfigValue{int64_t{100}}));
    const cfg::ConfigValue* v = t.find(42);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->as_int(), 100);
}

TEST(ConfigTable, FindMissingReturnsNull) {
    cfg::ConfigTable<8> t;
    EXPECT_EQ(t.find(999), nullptr);
}

TEST(ConfigTable, InsertOrAssignOverwrites) {
    cfg::ConfigTable<8> t;
    t.insert_or_assign(7, cfg::ConfigValue{int64_t{1}});
    t.insert_or_assign(7, cfg::ConfigValue{int64_t{2}});
    const cfg::ConfigValue* v = t.find(7);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->as_int(), 2);
}

TEST(ConfigTable, InsertIfAbsentDoesNotOverwrite) {
    cfg::ConfigTable<8> t;
    EXPECT_TRUE(t.insert_if_absent(3, cfg::ConfigValue{int64_t{1}}));
    EXPECT_FALSE(t.insert_if_absent(3, cfg::ConfigValue{int64_t{2}}));
    const cfg::ConfigValue* v = t.find(3);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->as_int(), 1);
}

TEST(ConfigTable, EraseRemovesEntry) {
    cfg::ConfigTable<8> t;
    t.insert_or_assign(5, cfg::ConfigValue{int64_t{9}});
    ASSERT_NE(t.find(5), nullptr);
    t.erase(5);
    EXPECT_EQ(t.find(5), nullptr);
}

TEST(ConfigTable, FullTableReturnsFalse) {
    cfg::ConfigTable<2> t;  // Cap=2 (power of two)
    EXPECT_TRUE(t.insert_or_assign(1, cfg::ConfigValue{int64_t{1}}));
    EXPECT_TRUE(t.insert_or_assign(2, cfg::ConfigValue{int64_t{2}}));
    // Third distinct key has no slot.
    EXPECT_FALSE(t.insert_or_assign(3, cfg::ConfigValue{int64_t{3}}));
}

// ============================================================================
// ConfigManager — set/get/defaults/contains/erase
// ============================================================================

TEST(ConfigManager, GetOrReturnsDefaultWhenAbsent) {
    cfg::ConfigManager cm;
    EXPECT_EQ(cm.get_or<int64_t>("missing.key", int64_t{42}), 42);
    EXPECT_FALSE(cm.contains("missing.key"));
}

TEST(ConfigManager, SetThenGetTyped) {
    cfg::ConfigManager cm;
    cm.set("server.port", cfg::ConfigValue{int64_t{9090}});
    EXPECT_TRUE(cm.contains("server.port"));
    EXPECT_EQ(cm.get_or<int64_t>("server.port", int64_t{0}), 9090);
}

TEST(ConfigManager, SetDefaultDoesNotOverwriteExisting) {
    cfg::ConfigManager cm;
    cm.set("a.b", cfg::ConfigValue{int64_t{1}});  // explicit set (highest priority)
    cm.set_default("a.b", cfg::ConfigValue{int64_t{99}});  // must not override
    EXPECT_EQ(cm.get_or<int64_t>("a.b", int64_t{0}), 1);
}

TEST(ConfigManager, SetDefaultUsedWhenAbsent) {
    cfg::ConfigManager cm;
    cm.set_default("workers", cfg::ConfigValue{int64_t{4}});
    EXPECT_EQ(cm.get_or<int64_t>("workers", int64_t{0}), 4);
}

TEST(ConfigManager, SetOverridesDefault) {
    cfg::ConfigManager cm;
    cm.set_default("x", cfg::ConfigValue{int64_t{1}});
    cm.set("x", cfg::ConfigValue{int64_t{2}});  // explicit override
    EXPECT_EQ(cm.get_or<int64_t>("x", int64_t{0}), 2);
}

TEST(ConfigManager, EraseRemovesKey) {
    cfg::ConfigManager cm;
    cm.set("k", cfg::ConfigValue{int64_t{1}});
    ASSERT_TRUE(cm.contains("k"));
    cm.erase("k");
    EXPECT_FALSE(cm.contains("k"));
}

TEST(ConfigManager, TypedDoubleAndBool) {
    cfg::ConfigManager cm;
    cm.set("ratio", cfg::ConfigValue{0.25});
    cm.set("enabled", cfg::ConfigValue{true});
    EXPECT_DOUBLE_EQ(cm.get_or<double>("ratio", 0.0), 0.25);
    EXPECT_TRUE(cm.get_or<bool>("enabled", false));
}

TEST(ConfigManager, StringViewAccessor) {
    cfg::ConfigManager cm;
    cm.set("name", cfg::ConfigValue{std::string_view{"qbuem"}});
    EXPECT_EQ(cm.get_or<std::string_view>("name", std::string_view{"default"}), "qbuem");
}

TEST(ConfigManager, StringCoercionToInt) {
    cfg::ConfigManager cm;
    // Values loaded from env/file are stored as strings; get_or<int64_t> parses.
    cm.set("port", cfg::ConfigValue{std::string_view{"8443"}});
    EXPECT_EQ(cm.get_or<int64_t>("port", int64_t{0}), 8443);
}

TEST(ConfigManager, StringCoercionToBoolVariants) {
    cfg::ConfigManager cm;
    cm.set("a", cfg::ConfigValue{std::string_view{"true"}});
    cm.set("b", cfg::ConfigValue{std::string_view{"1"}});
    cm.set("c", cfg::ConfigValue{std::string_view{"yes"}});
    cm.set("d", cfg::ConfigValue{std::string_view{"on"}});
    cm.set("e", cfg::ConfigValue{std::string_view{"false"}});
    EXPECT_TRUE(cm.get_or<bool>("a", false));
    EXPECT_TRUE(cm.get_or<bool>("b", false));
    EXPECT_TRUE(cm.get_or<bool>("c", false));
    EXPECT_TRUE(cm.get_or<bool>("d", false));
    EXPECT_FALSE(cm.get_or<bool>("e", true));
}

TEST(ConfigManager, StringCoercionToDouble) {
    cfg::ConfigManager cm;
    cm.set("rate", cfg::ConfigValue{std::string_view{"0.5"}});
    EXPECT_DOUBLE_EQ(cm.get_or<double>("rate", 0.0), 0.5);
}

TEST(ConfigManager, NonNumericStringFallsBackToDefault) {
    cfg::ConfigManager cm;
    cm.set("port", cfg::ConfigValue{std::string_view{"not-a-number"}});
    EXPECT_EQ(cm.get_or<int64_t>("port", int64_t{-1}), -1);
}

// ── get_secret — value AND error paths ─────────────────────────────────────

TEST(ConfigManager, GetSecretSuccess) {
    cfg::ConfigManager cm;
    cm.set("api.key", cfg::ConfigValue{std::string_view{"top-secret"}});
    auto r = cm.get_secret("api.key");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->reveal(), "top-secret");
    // Confirm redaction behaviour passes through the wrapper.
    EXPECT_EQ(std::format("{}", *r), "[REDACTED]");
}

TEST(ConfigManager, GetSecretMissingKeyIsError) {
    cfg::ConfigManager cm;
    auto r = cm.get_secret("absent");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error(), std::make_error_code(std::errc::no_such_file_or_directory));
}

TEST(ConfigManager, GetSecretWrongTypeIsError) {
    cfg::ConfigManager cm;
    cm.set("not.a.string", cfg::ConfigValue{int64_t{123}});
    auto r = cm.get_secret("not.a.string");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

// Note: ConfigManager::load_env is intentionally not exercised here — its
// `extern char** environ;` namespace-scoped declaration does not link on
// macOS (where `environ` must come via _NSGetEnviron()). The file lacks an
// `environ` definition in this namespace, so calling load_env() fails at
// link time on Darwin. The remaining ConfigManager surface is covered.

// ── load_file — value AND error paths ──────────────────────────────────────

TEST(ConfigManager, LoadFileMissingIsError) {
    cfg::ConfigManager cm;
    auto r = cm.load_file("/tmp/this-file-does-not-exist-qbuem-12345.ini");
    ASSERT_FALSE(r);
    // Error carries an errno-based error_code.
    EXPECT_NE(r.error().value(), 0);
}

TEST(ConfigManager, LoadFileParsesKeyValue) {
    const char* path = "/tmp/qbuem_cfg_tracing_test.ini";
    {
        FILE* f = ::fopen(path, "w");
        ASSERT_NE(f, nullptr);
        std::fputs("# a comment line\n", f);
        std::fputs("\n", f);                 // blank line ignored
        std::fputs("Server.Host = example.com\n", f);  // key lowercased, ws trimmed
        std::fputs("max_conns=128\n", f);
        std::fputs("no-equals-line\n", f);   // ignored (no '=')
        std::fclose(f);
    }
    cfg::ConfigManager cm;
    auto r = cm.load_file(path);
    ASSERT_TRUE(r);
    EXPECT_TRUE(cm.contains("server.host"));
    EXPECT_EQ(cm.get_or<std::string_view>("server.host", std::string_view{"?"}), "example.com");
    EXPECT_EQ(cm.get_or<int64_t>("max_conns", int64_t{0}), 128);
    EXPECT_FALSE(cm.contains("no-equals-line"));
    ::remove(path);
}

// ============================================================================
// trace_context.hpp — TraceId / SpanId / TraceContext
// ============================================================================

TEST(TraceId, DefaultIsInvalid) {
    tr::TraceId id;  // all-zero
    EXPECT_FALSE(id.is_valid());
}

TEST(TraceId, GenerateIsValidAndToChars) {
    tr::TraceId id = tr::TraceId::generate();
    EXPECT_TRUE(id.is_valid());
    char buf[33];
    size_t n = id.to_chars(buf, sizeof(buf));
    EXPECT_EQ(n, 32u);
    EXPECT_EQ(buf[32], '\0');
}

TEST(TraceId, ToCharsBufferTooSmall) {
    tr::TraceId id = tr::TraceId::generate();
    char small[10];
    EXPECT_EQ(id.to_chars(small, sizeof(small)), 0u);
}

TEST(SpanId, DefaultIsInvalid) {
    tr::SpanId id;
    EXPECT_FALSE(id.is_valid());
}

TEST(SpanId, GenerateIsValidAndToChars) {
    tr::SpanId id = tr::SpanId::generate();
    EXPECT_TRUE(id.is_valid());
    char buf[17];
    size_t n = id.to_chars(buf, sizeof(buf));
    EXPECT_EQ(n, 16u);
    EXPECT_EQ(buf[16], '\0');
}

TEST(SpanId, ToCharsBufferTooSmall) {
    tr::SpanId id = tr::SpanId::generate();
    char small[5];
    EXPECT_EQ(id.to_chars(small, sizeof(small)), 0u);
}

TEST(TraceContext, GenerateIsSampledAndValid) {
    tr::TraceContext ctx = tr::TraceContext::generate();
    EXPECT_TRUE(ctx.trace_id.is_valid());
    EXPECT_TRUE(ctx.parent_span_id.is_valid());
    EXPECT_TRUE(ctx.is_sampled());
    EXPECT_EQ(ctx.flags, 1u);
}

TEST(TraceContext, ChildSpanKeepsTraceIdNewSpanId) {
    tr::TraceContext root = tr::TraceContext::generate();
    tr::TraceContext child = root.child_span();
    EXPECT_EQ(child.trace_id.bytes, root.trace_id.bytes);
    EXPECT_NE(child.parent_span_id.bytes, root.parent_span_id.bytes);
    EXPECT_EQ(child.flags, root.flags);
}

TEST(TraceContext, ToTraceparentFormat) {
    tr::TraceContext ctx = tr::TraceContext::generate();
    std::string hdr = ctx.to_traceparent();
    EXPECT_EQ(hdr.size(), 55u);
    EXPECT_EQ(hdr.substr(0, 3), "00-");
    EXPECT_EQ(hdr[35], '-');
    EXPECT_EQ(hdr[52], '-');
}

TEST(TraceContext, RoundtripParseAndFormat) {
    tr::TraceContext ctx = tr::TraceContext::generate();
    std::string hdr = ctx.to_traceparent();
    auto r = tr::TraceContext::from_traceparent(hdr);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->trace_id.bytes, ctx.trace_id.bytes);
    EXPECT_EQ(r->parent_span_id.bytes, ctx.parent_span_id.bytes);
    EXPECT_EQ(r->flags, ctx.flags);
    // Format the parsed context again — must match exactly.
    EXPECT_EQ(r->to_traceparent(), hdr);
}

TEST(TraceContext, ParseKnownGoodVector) {
    // W3C spec example traceparent.
    std::string hdr = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
    auto r = tr::TraceContext::from_traceparent(hdr);
    ASSERT_TRUE(r);
    EXPECT_TRUE(r->is_sampled());
    EXPECT_EQ(r->to_traceparent(), hdr);
}

TEST(TraceContext, ParseUppercaseHexAccepted) {
    std::string hdr = "00-4BF92F3577B34DA6A3CE929D0E0E4736-00F067AA0BA902B7-01";
    auto r = tr::TraceContext::from_traceparent(hdr);
    ASSERT_TRUE(r);
    // Output is lowercased.
    EXPECT_EQ(r->to_traceparent(),
              "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01");
}

TEST(TraceContext, ParseTooShortIsError) {
    auto r = tr::TraceContext::from_traceparent("00-short");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(TraceContext, ParseWrongVersionIsError) {
    // Version "ff" is not "00".
    std::string hdr = "ff-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
    auto r = tr::TraceContext::from_traceparent(hdr);
    ASSERT_FALSE(r);
}

TEST(TraceContext, ParseMissingDelimiterIsError) {
    // Same length (55), but delimiter at index 35 replaced by a hex char.
    std::string hdr = "00-4bf92f3577b34da6a3ce929d0e0e4736000f067aa0ba902b7-01";
    auto r = tr::TraceContext::from_traceparent(hdr);
    ASSERT_FALSE(r);
}

TEST(TraceContext, ParseInvalidHexIsError) {
    // 'z' is not a hex digit inside the trace_id field.
    std::string hdr = "00-zbf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
    auto r = tr::TraceContext::from_traceparent(hdr);
    ASSERT_FALSE(r);
}

TEST(TraceContext, ParseAllZeroTraceIdIsError) {
    // All-zero trace_id is invalid per W3C.
    std::string hdr = "00-00000000000000000000000000000000-00f067aa0ba902b7-01";
    auto r = tr::TraceContext::from_traceparent(hdr);
    ASSERT_FALSE(r);
}

TEST(TraceContext, NotSampledFlag) {
    std::string hdr = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-00";
    auto r = tr::TraceContext::from_traceparent(hdr);
    ASSERT_TRUE(r);
    EXPECT_FALSE(r->is_sampled());
    EXPECT_EQ(r->flags, 0u);
}

// ============================================================================
// span.hpp — SpanData + Span (capturing exporter)
// ============================================================================

// Custom exporter that records exported spans for inspection.
class CapturingExporter final : public tr::SpanExporter {
public:
    void export_span(const tr::SpanData& span) override {
        names.push_back(span.name);
        statuses.push_back(span.status);
        last = span;
        ++count;
    }
    std::vector<std::string>     names;
    std::vector<tr::SpanStatus>  statuses;
    tr::SpanData                 last;
    int                          count{0};
};

TEST(SpanData, SetAttributeAddsAndOverwrites) {
    tr::SpanData d;
    d.set_attribute("queue", "orders");
    EXPECT_EQ(d.attribute_count, 1u);
    EXPECT_EQ(d.attributes[0].key, "queue");
    EXPECT_EQ(d.attributes[0].value, "orders");

    // Overwrite same key — count stays the same.
    d.set_attribute("queue", "payments");
    EXPECT_EQ(d.attribute_count, 1u);
    EXPECT_EQ(d.attributes[0].value, "payments");
}

TEST(SpanData, SetAttributeIgnoresEmptyKey) {
    tr::SpanData d;
    d.set_attribute("", "value");
    EXPECT_EQ(d.attribute_count, 0u);
}

TEST(SpanData, SetAttributeCapAtMax) {
    tr::SpanData d;
    for (int i = 0; i < 20; ++i) {  // kMaxAttributes = 16
        d.set_attribute(std::format("k{}", i), "v");
    }
    EXPECT_EQ(d.attribute_count, tr::SpanData::kMaxAttributes);
}

TEST(Span, ExportsOnDestruction) {
    auto exp = std::make_shared<CapturingExporter>();
    tr::Tracer tracer{exp};
    {
        tr::Span s = tracer.start_span("op", "pipeline", "action");
        s.set_status(tr::SpanStatus::Ok);
        s.set_attribute("k", "v");
    }  // destructor exports here
    ASSERT_EQ(exp->count, 1);
    EXPECT_EQ(exp->names[0], "op");
    EXPECT_EQ(exp->statuses[0], tr::SpanStatus::Ok);
    EXPECT_EQ(exp->last.pipeline_name, "pipeline");
    EXPECT_EQ(exp->last.action_name, "action");
    EXPECT_EQ(exp->last.attribute_count, 1u);
}

TEST(Span, ErrorStatusCarriesMessage) {
    auto exp = std::make_shared<CapturingExporter>();
    tr::Tracer tracer{exp};
    {
        tr::Span s = tracer.start_span("op", "p", "a");
        s.set_status(tr::SpanStatus::Error, "boom");
    }
    ASSERT_EQ(exp->count, 1);
    EXPECT_EQ(exp->last.status, tr::SpanStatus::Error);
    EXPECT_EQ(exp->last.error_message, "boom");
}

TEST(Span, NullTracerIsNoop) {
    // Construct a Span with no tracer — destruction must not crash.
    tr::SpanData d;
    d.name = "orphan";
    {
        tr::Span s{std::move(d), nullptr};
        s.set_attribute("a", "b");
    }
    SUCCEED();
}

TEST(Span, MovePreventsDoubleExport) {
    auto exp = std::make_shared<CapturingExporter>();
    tr::Tracer tracer{exp};
    {
        tr::Span a = tracer.start_span("moved", "p", "a");
        tr::Span b = std::move(a);  // a is now "ended"
        (void)b;
    }
    // Only one export despite two Span objects in scope.
    EXPECT_EQ(exp->count, 1);
}

TEST(Span, RootSpanHasInvalidParent) {
    auto exp = std::make_shared<CapturingExporter>();
    tr::Tracer tracer{exp};
    {
        tr::Span s = tracer.start_span("root", "p", "a");  // default empty parent
        (void)s;
    }
    ASSERT_EQ(exp->count, 1);
    EXPECT_TRUE(exp->last.trace_id.is_valid());
    EXPECT_FALSE(exp->last.parent_span_id.is_valid());  // root → invalid parent
}

TEST(Span, ChildSpanInheritsTraceId) {
    auto exp = std::make_shared<CapturingExporter>();
    tr::Tracer tracer{exp};
    tr::TraceContext parent = tr::TraceContext::generate();
    {
        tr::Span s = tracer.start_span("child", "p", "a", parent);
        (void)s;
    }
    ASSERT_EQ(exp->count, 1);
    EXPECT_EQ(exp->last.trace_id.bytes, parent.trace_id.bytes);
    EXPECT_EQ(exp->last.parent_span_id.bytes, parent.parent_span_id.bytes);
}

// ============================================================================
// exporter.hpp — NoopSpanExporter, LoggingSpanExporter, Tracer, PipelineTracer
// ============================================================================

TEST(NoopSpanExporter, DiscardsSilently) {
    tr::NoopSpanExporter noop;
    tr::SpanData d;
    d.name = "ignored";
    noop.export_span(d);  // no observable effect, must not crash
    noop.flush();
    noop.shutdown();
    SUCCEED();
}

TEST(Tracer, DefaultConstructsWithNoop) {
    tr::Tracer tracer;  // default → NoopSpanExporter
    {
        tr::Span s = tracer.start_span("op", "p", "a");
        (void)s;
    }  // no exporter capture, but must not crash
    SUCCEED();
}

TEST(Tracer, SetExporterReplaces) {
    tr::Tracer tracer;
    auto exp = std::make_shared<CapturingExporter>();
    tracer.set_exporter(exp);
    {
        tr::Span s = tracer.start_span("op", "p", "a");
        (void)s;
    }
    EXPECT_EQ(exp->count, 1);
}

TEST(Tracer, SetExporterNullptrFallsBackToNoop) {
    tr::Tracer tracer;
    tracer.set_exporter(nullptr);  // must fall back to Noop, not crash
    {
        tr::Span s = tracer.start_span("op", "p", "a");
        (void)s;
    }
    SUCCEED();
}

TEST(PipelineTracer, DefaultGlobalIsNoop) {
    // Without set_global_tracer, global() returns a Noop-backed tracer.
    auto& pt = tr::PipelineTracer::global();
    {
        tr::Span s = pt.start_span("op", "p", "a");
        (void)s;
    }
    SUCCEED();
}

TEST(PipelineTracer, SetExporterCapturesSpans) {
    tr::PipelineTracer pt;
    auto exp = std::make_shared<CapturingExporter>();
    pt.set_exporter(exp);
    {
        tr::Span s = pt.start_span("op", "pipe", "act");
        s.set_status(tr::SpanStatus::Ok);
    }
    ASSERT_EQ(exp->count, 1);
    EXPECT_EQ(exp->names[0], "op");
}

TEST(PipelineTracer, EndSpanDirectExport) {
    tr::PipelineTracer pt;
    auto exp = std::make_shared<CapturingExporter>();
    pt.set_exporter(exp);
    tr::SpanData d;
    d.name = "direct";
    pt.end_span(d);  // direct push (not via RAII)
    EXPECT_EQ(exp->count, 1);
    EXPECT_EQ(exp->names[0], "direct");
}

TEST(LoggingSpanExporter, DoesNotCrash) {
    // Writes to stderr; we only verify it runs cleanly.
    tr::LoggingSpanExporter logger;
    tr::SpanData d;
    d.name = "logged";
    d.pipeline_name = "p";
    d.action_name = "a";
    d.trace_id = tr::TraceId::generate();
    d.span_id = tr::SpanId::generate();
    d.status = tr::SpanStatus::Error;
    d.error_message = "an error";
    d.set_attribute("key", "val");
    d.start_time = std::chrono::system_clock::now();
    d.end_time = d.start_time + std::chrono::milliseconds{5};
    logger.export_span(d);
    SUCCEED();
}

// ============================================================================
// exporter.hpp — PrometheusTextExporter
// ============================================================================

TEST(PrometheusTextExporter, GaugeOutput) {
    tr::PrometheusTextExporter ex;
    ex.gauge("queue_depth", 42.0, "job=\"worker\"");
    std::string out = ex.export_text();
    EXPECT_NE(out.find("# TYPE queue_depth gauge"), std::string::npos);
    EXPECT_NE(out.find("queue_depth{job=\"worker\"}"), std::string::npos);
}

TEST(PrometheusTextExporter, CounterAccumulates) {
    tr::PrometheusTextExporter ex;
    ex.counter("messages", 10.0, "env=\"prod\"");
    ex.counter("messages", 5.0, "env=\"prod\"");  // same key accumulates → 15
    std::string out = ex.export_text();
    EXPECT_NE(out.find("messages_total counter"), std::string::npos);
    EXPECT_NE(out.find("15"), std::string::npos);
}

TEST(PrometheusTextExporter, HistogramSumAndCount) {
    tr::PrometheusTextExporter ex;
    ex.histogram("latency_ms", 100.0);
    ex.histogram("latency_ms", 200.0);
    std::string out = ex.export_text();
    EXPECT_NE(out.find("latency_ms histogram"), std::string::npos);
    EXPECT_NE(out.find("latency_ms_sum"), std::string::npos);
    EXPECT_NE(out.find("latency_ms_count"), std::string::npos);
}

TEST(PrometheusTextExporter, ExportClearsBuffer) {
    tr::PrometheusTextExporter ex;
    ex.gauge("g", 1.0);
    std::string first = ex.export_text();
    EXPECT_FALSE(first.empty());
    std::string second = ex.export_text();  // buffer cleared after first export
    EXPECT_TRUE(second.empty());
}

TEST(PrometheusTextExporter, FlushClearsEverything) {
    tr::PrometheusTextExporter ex;
    ex.counter("c", 3.0);
    ex.histogram("h", 1.0);
    ex.flush();  // discard without exporting
    std::string out = ex.export_text();
    EXPECT_TRUE(out.empty());
}

TEST(PrometheusTextExporter, EmptyLabelsRenderBraces) {
    tr::PrometheusTextExporter ex;
    ex.gauge("g_nolabel", 7.0);  // no labels → "{}"
    std::string out = ex.export_text();
    EXPECT_NE(out.find("g_nolabel{}"), std::string::npos);
}

// ============================================================================
// trace_logger.hpp — level_str, TraceLogRecord, TraceLogRing
// ============================================================================

TEST(LogLevel, LevelStr) {
    EXPECT_EQ(tr::level_str(tr::LogLevel::Trace), "TRACE");
    EXPECT_EQ(tr::level_str(tr::LogLevel::Debug), "DEBUG");
    EXPECT_EQ(tr::level_str(tr::LogLevel::Info),  "INFO ");
    EXPECT_EQ(tr::level_str(tr::LogLevel::Warn),  "WARN ");
    EXPECT_EQ(tr::level_str(tr::LogLevel::Error), "ERROR");
    EXPECT_EQ(tr::level_str(tr::LogLevel::Fatal), "FATAL");
}

TEST(TraceLogRecord, SetMessageTruncates) {
    tr::TraceLogRecord rec;
    std::string big(500, 'z');
    rec.set_message(big);
    // Stored as NUL-terminated within kMsgLen-1.
    std::string_view stored{rec.msg.data()};
    EXPECT_LE(stored.size(), tr::TraceLogRecord::kMsgLen - 1);
    EXPECT_EQ(stored.size(), tr::TraceLogRecord::kMsgLen - 1);
}

TEST(TraceLogRecord, SetShortMessage) {
    tr::TraceLogRecord rec;
    rec.set_message("hello");
    EXPECT_STREQ(rec.msg.data(), "hello");
}

TEST(TraceLogRing, PushPopFifo) {
    tr::TraceLogRing<4> ring;
    EXPECT_EQ(ring.size(), 0u);

    tr::TraceLogRecord a;
    a.set_message("first");
    a.trace_id = 1;
    tr::TraceLogRecord b;
    b.set_message("second");
    b.trace_id = 2;

    EXPECT_TRUE(ring.try_push(a));
    EXPECT_TRUE(ring.try_push(b));
    EXPECT_EQ(ring.size(), 2u);

    tr::TraceLogRecord out;
    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out.trace_id, 1u);
    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out.trace_id, 2u);
    EXPECT_FALSE(ring.try_pop(out));  // empty
}

TEST(TraceLogRing, FullReturnsFalse) {
    tr::TraceLogRing<2> ring;  // capacity 2
    tr::TraceLogRecord rec;
    EXPECT_TRUE(ring.try_push(rec));
    EXPECT_TRUE(ring.try_push(rec));
    EXPECT_FALSE(ring.try_push(rec));  // full
}

TEST(TraceLogRing, PopEmptyReturnsFalse) {
    tr::TraceLogRing<4> ring;
    tr::TraceLogRecord out;
    EXPECT_FALSE(ring.try_pop(out));
}

// ============================================================================
// lifecycle_tracer.hpp — SpanRecord, ShmSpanRing, ActiveSpan, LifecycleTracer
// ============================================================================

TEST(SpanRecord, SetNameTruncates) {
    tr::SpanRecord rec;
    std::string big(100, 'q');  // kNameLen = 56
    rec.set_name(big);
    std::string_view stored{rec.name.data()};
    EXPECT_EQ(stored.size(), tr::SpanRecord::kNameLen - 1);
}

TEST(SpanRecord, SetShortName) {
    tr::SpanRecord rec;
    rec.set_name("validate");
    EXPECT_STREQ(rec.name.data(), "validate");
}

TEST(SpanRecord, ExactlyTwoCacheLines) {
    EXPECT_EQ(sizeof(tr::SpanRecord), 128u);
}

TEST(ShmSpanRing, PushPopAndSize) {
    tr::ShmSpanRing<4> ring;
    EXPECT_EQ(ring.size(), 0u);
    EXPECT_EQ(ring.try_pop(), nullptr);

    tr::SpanRecord rec;
    rec.span_id = 7;
    EXPECT_TRUE(ring.try_push(rec));
    EXPECT_EQ(ring.size(), 1u);

    const tr::SpanRecord* got = ring.try_pop();
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->span_id, 7u);
    EXPECT_EQ(ring.size(), 0u);
}

TEST(ShmSpanRing, FullReturnsFalse) {
    tr::ShmSpanRing<2> ring;
    tr::SpanRecord rec;
    EXPECT_TRUE(ring.try_push(rec));
    EXPECT_TRUE(ring.try_push(rec));
    EXPECT_FALSE(ring.try_push(rec));  // full
}

TEST(LifecycleTracer, StartLifecycleEmitsRootSpan) {
    tr::LifecycleTracer<8> tracer{"order-service"};
    EXPECT_EQ(tracer.total_spans(), 0u);
    {
        tr::ActiveSpan span = tracer.start_lifecycle("process_order");
        EXPECT_EQ(tracer.total_spans(), 1u);
        span.end(tr::SpanStatus::Ok);  // explicit end → flush to ring
    }
    EXPECT_EQ(tracer.buffered_spans(), 1u);
}

TEST(LifecycleTracer, AutoEndOnDestruction) {
    tr::LifecycleTracer<8> tracer{"svc"};
    {
        tr::ActiveSpan span = tracer.start_lifecycle("auto");
        // no explicit end() → destructor calls end(SpanStatus::Ok)
        (void)span;
    }
    EXPECT_EQ(tracer.buffered_spans(), 1u);
}

TEST(LifecycleTracer, ContextHasValidTraceId) {
    tr::LifecycleTracer<8> tracer{"svc"};
    tr::ActiveSpan span = tracer.start_lifecycle("ctx-test");
    tr::TraceContext ctx = span.context();
    EXPECT_TRUE(ctx.trace_id.is_valid());
    EXPECT_EQ(ctx.flags, 1u);  // sampled
    span.end();
}

TEST(LifecycleTracer, DrainReadsRecords) {
    tr::LifecycleTracer<8> tracer{"svc"};
    {
        tr::ActiveSpan span = tracer.start_lifecycle("drained");
        span.end(tr::SpanStatus::Error);
    }
    ASSERT_EQ(tracer.buffered_spans(), 1u);

    int drained = 0;
    uint8_t status = 0;
    tracer.drain([&](const tr::SpanRecord& rec) {
        ++drained;
        status = rec.status;
        return true;  // continue
    });
    EXPECT_EQ(drained, 1);
    EXPECT_EQ(status, static_cast<uint8_t>(tr::SpanStatus::Error));
    EXPECT_EQ(tracer.buffered_spans(), 0u);  // ring drained
}

TEST(LifecycleTracer, MoveConstructTransfersSpan) {
    tr::LifecycleTracer<8> tracer{"svc"};
    {
        tr::ActiveSpan a = tracer.start_lifecycle("moved");
        tr::ActiveSpan b = std::move(a);
        b.end(tr::SpanStatus::Ok);
        // a is moved-from; its destructor must be a no-op (no double push).
    }
    EXPECT_EQ(tracer.buffered_spans(), 1u);
}

TEST(LifecycleTracer, DoubleEndIsIdempotent) {
    tr::LifecycleTracer<8> tracer{"svc"};
    tr::ActiveSpan span = tracer.start_lifecycle("idem");
    span.end(tr::SpanStatus::Ok);
    span.end(tr::SpanStatus::Error);  // second end is ignored
    EXPECT_EQ(tracer.buffered_spans(), 1u);
}

TEST(LifecycleTracer, StatsCounters) {
    tr::LifecycleTracer<2> tracer{"svc"};
    EXPECT_EQ(tracer.total_spans(), 0u);
    EXPECT_EQ(tracer.dropped_spans(), 0u);
    {
        tr::ActiveSpan s = tracer.start_lifecycle("one");
        s.end();
    }
    EXPECT_EQ(tracer.total_spans(), 1u);
}
