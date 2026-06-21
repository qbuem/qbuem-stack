/**
 * @file tests/otlp_exporter_test.cpp
 * @brief Tests for the OTLP/JSON encoder and OtlpHttpExporter.
 */

#include <qbuem/tracing/otlp_exporter.hpp>

#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

using namespace qbuem::tracing;

namespace {

std::chrono::system_clock::time_point tp(uint64_t ns) {
  return std::chrono::system_clock::time_point(
      std::chrono::duration_cast<std::chrono::system_clock::duration>(
          std::chrono::nanoseconds(ns)));
}

SpanData make_span() {
  SpanData s;
  s.trace_id.bytes = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                      0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10};
  s.span_id.bytes = {0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8};
  // parent_span_id left all-zero → root (invalid)
  s.name          = "process";
  s.pipeline_name = "ingest";
  s.action_name   = "parse";
  s.start_time    = tp(1700000000000000000ULL); // microsecond-aligned → exact on both libc++/libstdc++
  s.end_time      = tp(1700000000123456000ULL);
  s.status        = SpanStatus::Ok;
  s.set_attribute("region", "us");
  return s;
}

bool has(const std::string& hay, std::string_view needle) {
  return hay.find(needle) != std::string::npos;
}

std::string encode_one(const SpanData& s, std::string_view svc) {
  return encode_otlp_traces_json(std::span<const SpanData>(&s, 1), svc);
}

} // namespace

TEST(OtlpEncode, CoreFields) {
  const std::string j = encode_one(make_span(), "orders-svc");
  EXPECT_TRUE(has(j, "\"resourceSpans\":["));
  EXPECT_TRUE(has(j, "\"service.name\""));
  EXPECT_TRUE(has(j, "\"stringValue\":\"orders-svc\""));
  EXPECT_TRUE(has(j, "\"traceId\":\"0102030405060708090a0b0c0d0e0f10\""));
  EXPECT_TRUE(has(j, "\"spanId\":\"a1a2a3a4a5a6a7a8\""));
  EXPECT_TRUE(has(j, "\"name\":\"process\""));
  EXPECT_TRUE(has(j, "\"kind\":1"));
  EXPECT_TRUE(has(j, "\"startTimeUnixNano\":\"1700000000000000000\""));
  EXPECT_TRUE(has(j, "\"endTimeUnixNano\":\"1700000000123456000\""));
  EXPECT_TRUE(has(j, "\"status\":{\"code\":1}"));
  // pipeline/action + custom attribute
  EXPECT_TRUE(has(j, "\"qbuem.pipeline\""));
  EXPECT_TRUE(has(j, "\"stringValue\":\"ingest\""));
  EXPECT_TRUE(has(j, "\"qbuem.action\""));
  EXPECT_TRUE(has(j, "\"key\":\"region\""));
  EXPECT_TRUE(has(j, "\"stringValue\":\"us\""));
}

TEST(OtlpEncode, RootSpanOmitsParent) {
  EXPECT_FALSE(has(encode_one(make_span(), "svc"), "parentSpanId"));
}

TEST(OtlpEncode, ChildSpanIncludesParent) {
  SpanData s = make_span();
  s.parent_span_id.bytes = {0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8};
  EXPECT_TRUE(has(encode_one(s, "svc"), "\"parentSpanId\":\"b1b2b3b4b5b6b7b8\""));
}

TEST(OtlpEncode, EscapesStrings) {
  SpanData s = make_span();
  s.name = "say \"hi\"\nline";
  const std::string j = encode_one(s, "svc");
  EXPECT_TRUE(has(j, "say \\\"hi\\\"\\nline")); // quotes + newline escaped
  EXPECT_FALSE(has(j, "say \"hi\""));           // raw unescaped must not appear
}

TEST(OtlpEncode, ErrorStatusCarriesMessage) {
  SpanData s = make_span();
  s.status = SpanStatus::Error;
  s.error_message = "boom";
  const std::string j = encode_one(s, "svc");
  EXPECT_TRUE(has(j, "\"status\":{\"code\":2,\"message\":\"boom\"}"));
}

TEST(OtlpEncode, MultipleSpansInOneRequest) {
  SpanData a = make_span();
  SpanData b = make_span();
  b.span_id.bytes = {0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8};
  std::array<SpanData, 2> arr = {a, b};
  const std::string j =
      encode_otlp_traces_json(std::span<const SpanData>(arr.data(), 2), "svc");
  EXPECT_TRUE(has(j, "a1a2a3a4a5a6a7a8"));
  EXPECT_TRUE(has(j, "c1c2c3c4c5c6c7c8"));
}

// ── Exporter ───────────────────────────────────────────────────────────────

namespace {
struct Capture {
  std::mutex m;
  std::vector<std::string> bodies;
  OtlpHttpExporter::Transport transport() {
    return [this](std::string_view body) {
      std::lock_guard lk(m);
      bodies.emplace_back(body);
    };
  }
  size_t count() { std::lock_guard lk(m); return bodies.size(); }
  std::string joined() {
    std::lock_guard lk(m);
    std::string s;
    for (auto& b : bodies) s += b;
    return s;
  }
};
// Large flush interval so ONLY explicit flush()/shutdown() send (deterministic).
OtlpHttpExporter::Config quiet_cfg() {
  return {.service_name = "svc", .flush_interval = std::chrono::hours{1}};
}
} // namespace

TEST(OtlpExporter, FlushSendsViaTransport) {
  auto cap = std::make_shared<Capture>();
  OtlpHttpExporter exp(cap->transport(), quiet_cfg());
  exp.export_span(make_span());
  EXPECT_EQ(cap->count(), 0u); // nothing sent until flush
  exp.flush();
  ASSERT_EQ(cap->count(), 1u);
  EXPECT_TRUE(has(cap->joined(), "0102030405060708090a0b0c0d0e0f10"));
}

TEST(OtlpExporter, ShutdownFlushesPending) {
  auto cap = std::make_shared<Capture>();
  {
    OtlpHttpExporter exp(cap->transport(), quiet_cfg());
    exp.export_span(make_span());
    exp.shutdown(); // must flush the pending span
  }
  EXPECT_GE(cap->count(), 1u);
  EXPECT_TRUE(has(cap->joined(), "a1a2a3a4a5a6a7a8"));
}

TEST(OtlpExporter, DropsBeyondQueueCap) {
  auto cap = std::make_shared<Capture>();
  OtlpHttpExporter::Config cfg = quiet_cfg();
  cfg.max_queue = 2;
  OtlpHttpExporter exp(cap->transport(), cfg);
  for (int i = 0; i < 5; ++i) exp.export_span(make_span());
  EXPECT_EQ(exp.dropped(), 3u); // only 2 retained
  exp.flush();
  EXPECT_EQ(cap->count(), 1u);  // the 2 retained sent in one batch
}
