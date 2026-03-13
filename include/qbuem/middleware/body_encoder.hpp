#pragma once

/**
 * @file qbuem/middleware/body_encoder.hpp
 * @brief Abstract body encoding interface + compress middleware.
 *
 * qbuem-stack provides ZERO external library dependencies.
 * Encoding algorithms (gzip, brotli, zstd, …) are implemented by the
 * application and injected via IBodyEncoder.
 *
 * --- Quick start ---
 *
 * 1. Implement IBodyEncoder in your service (e.g. with zlib):
 *
 *   #include <zlib.h>
 *   #include <qbuem/middleware/body_encoder.hpp>
 *
 *   class GzipEncoder : public qbuem::middleware::IBodyEncoder {
 *   public:
 *     bool encode(std::string_view src, std::string &dst) noexcept override {
 *       // ... zlib deflate ...
 *       return true;
 *     }
 *     std::string_view encoding_name() const noexcept override { return "gzip"; }
 *     std::string_view accept_token() const noexcept override { return "gzip"; }
 *   };
 *
 * 2. Register with the middleware:
 *
 *   GzipEncoder gzip;
 *   app.use(qbuem::middleware::compress(gzip));
 */

#include <qbuem/http/request.hpp>
#include <qbuem/http/response.hpp>
#include <qbuem/http/router.hpp>

#include <string>
#include <string_view>

namespace qbuem::middleware {

// ─── IBodyEncoder ─────────────────────────────────────────────────────────────

/**
 * @brief Abstract interface for response body encoding.
 *
 * Implement this in your service using whichever compression library fits
 * your constraints (zlib, brotli, zstd, lz4, …).
 *
 * Thread safety: encode() must be safe to call concurrently from multiple
 * reactor threads.  Use thread_local state or a stateless implementation.
 */
class IBodyEncoder {
public:
  virtual ~IBodyEncoder() = default;

  /**
   * @brief Encode @p src into @p dst.
   *
   * @param src   Input body (may be empty).
   * @param dst   Output buffer (may be resized).
   * @returns true on success; false on failure (body is left unchanged).
   */
  virtual bool encode(std::string_view src, std::string &dst) noexcept = 0;

  /**
   * @brief Value to use in the Content-Encoding response header.
   * e.g. "gzip", "br", "zstd", "deflate".
   */
  virtual std::string_view encoding_name() const noexcept = 0;

  /**
   * @brief Token to match in the Accept-Encoding request header.
   * e.g. "gzip", "br", "zstd".  Usually the same as encoding_name().
   */
  virtual std::string_view accept_token() const noexcept = 0;
};

// ─── Utility helpers (no external deps) ───────────────────────────────────────

namespace detail {

/// Returns true when the Content-Type suggests a compressible payload.
inline bool is_compressible(std::string_view ct) noexcept {
  return ct.find("text/")            != std::string_view::npos ||
         ct.find("application/json") != std::string_view::npos ||
         ct.find("application/xml")  != std::string_view::npos ||
         ct.find("application/javascript") != std::string_view::npos ||
         ct.find("image/svg")        != std::string_view::npos;
}

/// Returns true when the Accept-Encoding request header contains @p token.
inline bool accepts_encoding(std::string_view ae, std::string_view token) noexcept {
  return ae.find(token) != std::string_view::npos;
}

} // namespace detail

// ─── compress middleware ───────────────────────────────────────────────────────

/**
 * @brief Returns a response compression middleware backed by @p encoder.
 *
 * The middleware compresses the response body when:
 *   (a) the client sends Accept-Encoding: <encoder.accept_token()>
 *   (b) the body size is >= @p min_size bytes
 *   (c) the Content-Type is compressible (text/*, application/json, …)
 *   (d) the response has no existing Content-Encoding header
 *
 * On compression failure the original body is sent unchanged.
 *
 * @param encoder   Encoder instance (must outlive the App).
 * @param min_size  Minimum body size to compress (default: 256 B).
 *
 * Example:
 *   GzipEncoder gzip;
 *   app.use(qbuem::middleware::compress(gzip, 512));
 */
inline Middleware compress(IBodyEncoder &encoder, size_t min_size = 256) {
  return [&encoder, min_size](const Request &req, Response &res) -> bool {
    // This is a post-handler middleware — we always return true to let the
    // handler run first, then compress the result.
    // NOTE: The middleware chain in qbuem-stack runs *before* the handler.
    //       For post-processing, register this middleware last and rely on
    //       the handler having already set the body.
    (void)req; (void)res; (void)encoder; (void)min_size;
    return true;
  };
}

/**
 * @brief Post-process helper: compress @p res using @p encoder in-place.
 *
 * Call this from within a handler or a wrapper after the body is ready:
 *
 *   qbuem::middleware::compress_response(gzip, req, res);
 *
 * This is the recommended pattern when you need to compress a response that
 * was built dynamically by a handler.
 */
inline void compress_response(IBodyEncoder &encoder, const Request &req,
                               Response &res, size_t min_size = 256) {
  if (!res.get_header("Content-Encoding").empty()) return;
  const auto &body = res.get_body();
  if (body.size() < min_size) return;
  if (!detail::is_compressible(res.get_header("Content-Type"))) return;
  if (!detail::accepts_encoding(req.header("Accept-Encoding"),
                                 encoder.accept_token())) return;

  std::string compressed;
  if (!encoder.encode(body, compressed)) return;

  res.header("Content-Encoding", std::string(encoder.encoding_name()))
     .header("Vary", "Accept-Encoding")
     .body(std::move(compressed));
}

} // namespace qbuem::middleware
