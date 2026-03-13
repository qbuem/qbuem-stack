#pragma once

/**
 * @file draco/middleware/compress.hpp
 * @brief Gzip/deflate response compression middleware.
 *
 * Usage:
 *   #include <draco/middleware/compress.hpp>
 *   app.use(draco::middleware::compress());
 *
 * Behaviour:
 *   - Inspects Accept-Encoding request header for "gzip".
 *   - Compresses response body with zlib when:
 *     (a) client accepts gzip, AND
 *     (b) response body is >= kMinSizeBytes (default 256 B), AND
 *     (c) response Content-Type is compressible (text/*, application/json, …).
 *   - Sets Content-Encoding: gzip and updates Content-Length.
 *   - Falls back gracefully if zlib is unavailable at compile time.
 */

#include <draco/http/request.hpp>
#include <draco/http/response.hpp>
#include <draco/http/router.hpp>

#include <string>
#include <string_view>

#ifdef DRACO_NO_ZLIB
// Allow builds without zlib by defining DRACO_NO_ZLIB.
#else
#  include <zlib.h>
#  define DRACO_HAS_ZLIB 1
#endif

namespace draco::middleware {

namespace detail {

// Minimum response body size to bother compressing (bytes).
inline constexpr size_t kCompressMinSize = 256;

// Return true if the MIME type is worth compressing.
inline bool is_compressible(std::string_view content_type) noexcept {
  // text/* and common application types
  if (content_type.starts_with("text/"))             return true;
  if (content_type.starts_with("application/json"))  return true;
  if (content_type.starts_with("application/xml"))   return true;
  if (content_type.starts_with("application/javascript")) return true;
  if (content_type.starts_with("image/svg"))         return true;
  return false;
}

// Return true if the Accept-Encoding header contains "gzip".
inline bool accepts_gzip(std::string_view accept_enc) noexcept {
  return accept_enc.find("gzip") != std::string_view::npos;
}

#ifdef DRACO_HAS_ZLIB
// Compress src with gzip.  Returns compressed bytes or empty on error.
inline std::string gzip_compress(std::string_view src) {
  z_stream zs{};
  // windowBits = 15 + 16 → gzip header/trailer
  if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                   15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
    return {};

  zs.next_in  = reinterpret_cast<Bytef *>(const_cast<char *>(src.data()));
  zs.avail_in = static_cast<uInt>(src.size());

  std::string out;
  out.resize(deflateBound(&zs, static_cast<uLong>(src.size())));

  zs.next_out  = reinterpret_cast<Bytef *>(out.data());
  zs.avail_out = static_cast<uInt>(out.size());

  int ret = deflate(&zs, Z_FINISH);
  deflateEnd(&zs);

  if (ret != Z_STREAM_END) return {};
  out.resize(zs.total_out);
  return out;
}
#endif // DRACO_HAS_ZLIB

} // namespace detail

/**
 * @brief Create a gzip compression middleware.
 *
 * Returns a Middleware that compresses response bodies for clients that
 * send Accept-Encoding: gzip.  Compatible with App::use().
 *
 * @param min_size  Minimum body size (bytes) to trigger compression.
 *                  Defaults to 256 B.
 */
inline Middleware compress(size_t min_size = detail::kCompressMinSize) {
  return [min_size](const Request &req, Response &res, std::function<void()> next) {
    next(); // let the handler run first

#ifdef DRACO_HAS_ZLIB
    // Skip if already encoded or body too small.
    if (!res.get_header("Content-Encoding").empty()) return;

    std::string_view body = res.get_body();
    if (body.size() < min_size)                      return;

    // Skip non-compressible content types.
    std::string_view ct = res.get_header("Content-Type");
    if (!detail::is_compressible(ct))                return;

    // Skip if client doesn't accept gzip.
    if (!detail::accepts_gzip(req.header("Accept-Encoding"))) return;

    // Compress the body.
    std::string compressed = detail::gzip_compress(body);
    if (compressed.empty()) return; // zlib error — send uncompressed

    res.header("Content-Encoding", "gzip");
    res.header("Vary", "Accept-Encoding");
    res.body(compressed);
#else
    (void)req; (void)res; (void)next; (void)min_size;
#endif
  };
}

} // namespace draco::middleware
