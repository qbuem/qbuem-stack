#pragma once

/**
 * @file qbuem/http/trace_middleware.hpp
 * @brief HTTP traceparent header automatic parsing and traceresponse injection middleware
 * @defgroup qbuem_trace_middleware HTTP Trace Middleware
 * @ingroup qbuem_http
 *
 * According to the W3C Trace Context Level 1 standard (https://www.w3.org/TR/trace-context/):
 * - Parse the inbound `traceparent` header and create a `TraceContext`
 * - Create a child span and store it in `Context` as a `TraceContextSlot`
 * - Add the `traceresponse` header to the outbound response
 *
 * ## Example: registering with a Router
 * ```cpp
 * qbuem::http::Router router;
 * router.use_async(qbuem::http::make_trace_middleware());
 * ```
 *
 * ## Accessing TraceContext in a Pipeline
 * ```cpp
 * // Inside Action::push()
 * if (auto slot = env.ctx.get<qbuem::TraceContextSlot>()) {
 *     auto child = slot->value.child_span();
 *     // child.to_traceparent() — propagate to the next service
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
 * @brief Configuration for trace_middleware.
 */
struct TraceMiddlewareConfig {
  /// Sampler — uses AlwaysSampler when nullptr.
  std::shared_ptr<tracing::Sampler> sampler;

  /// Whether to add the traceresponse header (default: true).
  bool add_traceresponse{true};

  /// Whether to also parse legacy headers such as `X-B3-TraceId` as a fallback.
  bool parse_b3_fallback{false};
};

// ---------------------------------------------------------------------------
// make_trace_middleware
// ---------------------------------------------------------------------------

/**
 * @brief Factory for the W3C traceparent parsing + traceresponse injection async middleware.
 *
 * Register the returned middleware via `Router::use_async()`.
 *
 * @param cfg Configuration (defaults to AlwaysSampler + traceresponse enabled).
 * @returns AsyncMiddleware function object.
 */
[[nodiscard]] inline AsyncMiddleware make_trace_middleware(
    TraceMiddlewareConfig cfg = {}) {

  // Set default sampler
  if (!cfg.sampler) {
    cfg.sampler = std::make_shared<tracing::AlwaysSampler>();
  }

  return [cfg = std::move(cfg)](const Request &req, Response &res,
                                NextFn next) -> Task<bool> {
    tracing::TraceContext trace_ctx;
    bool has_parent = false;

    // ── 1. Parse traceparent ─────────────────────────────────────────────────
    std::string_view traceparent = req.header("traceparent");
    if (!traceparent.empty()) {
      auto result = tracing::TraceContext::from_traceparent(traceparent);
      if (result) {
        // Create a child span from the parent span
        trace_ctx = result->child_span();
        has_parent = true;
      }
    }

    // ── 2. B3 header fallback (optional) ────────────────────────────────────
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
        // Generate a new span_id (as a child span)
        trace_ctx = trace_ctx.child_span();
        has_parent = true;
      }
    }

    // ── 3. Create root span (when no traceparent is present) ───────────────────────────
    if (!has_parent) {
      trace_ctx = tracing::TraceContext::generate();
    }

    // ── 4. Sampling decision ─────────────────────────────────────────────────────
    tracing::SamplingContext sample_ctx{
        .pipeline_name = "http",
        .action_name   = "request",
        .span_name     = req.path(),
        .parent        = has_parent ? nullptr : nullptr,  // parent is already reflected
    };
    auto decision = cfg.sampler->should_sample(sample_ctx);
    if (decision == tracing::SamplingDecision::DROP) {
      trace_ctx.flags &= ~0x01u;  // clear sampled bit
    } else {
      trace_ctx.flags |= 0x01u;   // set sampled bit
    }

    // ── 5. Record span start in PipelineTracer ─────────────────────────────────
    tracing::PipelineTracer::global().start_span(
        "http", "request", req.path());

    // ── 6. Execute next handler ────────────────────────────────────────────
    bool result = co_await next();

    // ── 7. End span ────────────────────────────────────────────────────────
    tracing::PipelineTracer::global().end_span("http", "request", req.path());

    // ── 8. Add traceresponse header ─────────────────────────────────────────
    if (cfg.add_traceresponse) {
      // W3C traceresponse: same traceparent format
      res.header("traceresponse", trace_ctx.to_traceparent());
    }

    co_return result;
  };
}

} // namespace qbuem::http

/** @} */
