#pragma once

/**
 * @file qbuem/http/trace_middleware.hpp
 * @brief HTTP traceparent 헤더 자동 파싱 및 traceresponse 주입 미들웨어
 * @defgroup qbuem_trace_middleware HTTP Trace Middleware
 * @ingroup qbuem_http
 *
 * W3C Trace Context Level 1 표준(https://www.w3.org/TR/trace-context/)에 따라:
 * - 인바운드 `traceparent` 헤더를 파싱하여 `TraceContext` 생성
 * - 자식 스팬을 생성하고 `Context`에 `TraceContextSlot`으로 저장
 * - 아웃바운드 응답에 `traceresponse` 헤더 추가
 *
 * ## Router에 등록하는 예시
 * ```cpp
 * qbuem::http::Router router;
 * router.use_async(qbuem::http::make_trace_middleware());
 * ```
 *
 * ## Pipeline에서 TraceContext 접근
 * ```cpp
 * // Action::push() 내부에서
 * if (auto slot = env.ctx.get<qbuem::TraceContextSlot>()) {
 *     auto child = slot->value.child_span();
 *     // child.to_traceparent() — 다음 서비스로 전파
 * }
 * ```
 * @{
 */

#include <qbuem/http/request.hpp>
#include <qbuem/http/response.hpp>
#include <qbuem/http/router.hpp>
#include <qbuem/tracing/exporter.hpp>
#include <qbuem/tracing/sampler.hpp>
#include <qbuem/tracing/trace_context.hpp>

#include <functional>
#include <memory>
#include <string>

namespace qbuem::http {

// ---------------------------------------------------------------------------
// TraceMiddlewareConfig
// ---------------------------------------------------------------------------

/**
 * @brief trace_middleware 설정.
 */
struct TraceMiddlewareConfig {
  /// 샘플러 — nullptr이면 AlwaysSampler 사용.
  std::shared_ptr<tracing::Sampler> sampler;

  /// traceresponse 헤더 추가 여부 (기본 true).
  bool add_traceresponse{true};

  /// `X-B3-TraceId` 등 레거시 헤더도 fallback으로 파싱할지 여부.
  bool parse_b3_fallback{false};
};

// ---------------------------------------------------------------------------
// make_trace_middleware
// ---------------------------------------------------------------------------

/**
 * @brief W3C traceparent 파싱 + traceresponse 주입 비동기 미들웨어 팩토리.
 *
 * 생성된 미들웨어는 `Router::use_async()`에 등록합니다.
 *
 * @param cfg 설정 (기본값으로 AlwaysSampler + traceresponse 활성화).
 * @returns AsyncMiddleware 함수 객체.
 */
[[nodiscard]] inline AsyncMiddleware make_trace_middleware(
    TraceMiddlewareConfig cfg = {}) {

  // 기본 샘플러 설정
  if (!cfg.sampler) {
    cfg.sampler = std::make_shared<tracing::AlwaysSampler>();
  }

  return [cfg = std::move(cfg)](const Request &req, Response &res,
                                NextFn next) -> Task<bool> {
    tracing::TraceContext trace_ctx;
    bool has_parent = false;

    // ── 1. traceparent 파싱 ─────────────────────────────────────────────────
    std::string_view traceparent = req.header("traceparent");
    if (!traceparent.empty()) {
      auto result = tracing::TraceContext::from_traceparent(traceparent);
      if (result) {
        // 부모 스팬에서 자식 스팬 생성
        trace_ctx = result->child_span();
        has_parent = true;
      }
    }

    // ── 2. B3 헤더 fallback (선택적) ────────────────────────────────────────
    if (!has_parent && cfg.parse_b3_fallback) {
      std::string_view b3_traceid = req.header("X-B3-TraceId");
      std::string_view b3_spanid  = req.header("X-B3-SpanId");
      std::string_view b3_sampled = req.header("X-B3-Sampled");

      if (!b3_traceid.empty() && b3_traceid.size() == 32) {
        // B3 TraceId (32 hex chars = 16 bytes)
        auto &tid = trace_ctx.trace_id.bytes;
        for (int i = 0; i < 16; ++i) {
          auto h = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
            return 0;
          };
          tid[i] = static_cast<uint8_t>(
              (h(b3_traceid[i * 2]) << 4) | h(b3_traceid[i * 2 + 1]));
        }

        if (!b3_spanid.empty() && b3_spanid.size() == 16) {
          auto &sid = trace_ctx.parent_span_id.bytes;
          for (int i = 0; i < 8; ++i) {
            auto h = [](char c) -> uint8_t {
              if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
              if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
              if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
              return 0;
            };
            sid[i] = static_cast<uint8_t>(
                (h(b3_spanid[i * 2]) << 4) | h(b3_spanid[i * 2 + 1]));
          }
        }

        trace_ctx.flags = (b3_sampled == "1") ? 0x01 : 0x00;
        // 새 span_id 생성 (child span처럼)
        trace_ctx = trace_ctx.child_span();
        has_parent = true;
      }
    }

    // ── 3. 루트 스팬 생성 (traceparent 없는 경우) ───────────────────────────
    if (!has_parent) {
      trace_ctx = tracing::TraceContext::generate();
    }

    // ── 4. 샘플링 결정 ─────────────────────────────────────────────────────
    tracing::SamplingContext sample_ctx{
        .pipeline_name = "http",
        .action_name   = "request",
        .span_name     = req.path(),
        .parent        = has_parent ? nullptr : nullptr,  // 부모는 이미 반영됨
    };
    auto decision = cfg.sampler->should_sample(sample_ctx);
    if (decision == tracing::SamplingDecision::DROP) {
      trace_ctx.flags &= ~0x01u;  // sampled bit 클리어
    } else {
      trace_ctx.flags |= 0x01u;   // sampled bit 설정
    }

    // ── 5. PipelineTracer에 스팬 시작 기록 ─────────────────────────────────
    tracing::PipelineTracer::global().start_span(
        "http", "request", req.path());

    // ── 6. 다음 핸들러 실행 ────────────────────────────────────────────────
    bool result = co_await next();

    // ── 7. 스팬 종료 ───────────────────────────────────────────────────────
    tracing::PipelineTracer::global().end_span("http", "request", req.path());

    // ── 8. traceresponse 헤더 추가 ─────────────────────────────────────────
    if (cfg.add_traceresponse) {
      // W3C traceresponse: 동일 traceparent 형식
      res.header("traceresponse", trace_ctx.to_traceparent());
    }

    co_return result;
  };
}

} // namespace qbuem::http

/** @} */
