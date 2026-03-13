#pragma once

/**
 * @file cors.hpp
 * @brief CORS (Cross-Origin Resource Sharing) middleware for Draco WAS.
 *
 * Usage:
 *
 *   #include <qbuem/middleware/cors.hpp>
 *
 *   // Allow all origins (development / public API)
 *   app.use(qbuem::middleware::cors());
 *
 *   // Restrict to specific origin
 *   app.use(qbuem::middleware::cors({
 *       .allow_origin      = "https://example.com",
 *       .allow_methods     = "GET, POST, PUT, DELETE",
 *       .allow_headers     = "Content-Type, Authorization",
 *       .allow_credentials = true,
 *       .max_age           = 3600,
 *   }));
 *
 * Preflight (OPTIONS) requests are handled automatically: the middleware
 * returns a 204 No Content response and halts the middleware chain.
 */

#include <qbuem/http/request.hpp>
#include <qbuem/http/response.hpp>
#include <qbuem/http/router.hpp>

#include <string>
#include <unordered_set>
#include <vector>

namespace qbuem::middleware {

/** @brief Configuration for the CORS middleware. */
struct CorsConfig {
  /**
   * Value of Access-Control-Allow-Origin. Use "*" for public APIs.
   *
   * When allow_origins (whitelist) is set, this field is ignored and the
   * reflected request Origin is returned when it matches the whitelist.
   */
  std::string allow_origin  = "*";

  /**
   * Dynamic origin whitelist.  When non-empty, the middleware reflects the
   * request Origin header only if it appears in this set.  Origins not in
   * the whitelist get no CORS headers and the chain continues.
   *
   * Example:
   *   qbuem::middleware::CorsConfig cfg;
   *   cfg.allow_origins = {"https://app.example.com", "https://admin.example.com"};
   */
  std::vector<std::string> allow_origins;

  /** Comma-separated list of allowed HTTP methods. */
  std::string allow_methods = "GET, POST, PUT, DELETE, PATCH, OPTIONS, HEAD";

  /** Comma-separated list of allowed request headers. */
  std::string allow_headers = "Content-Type, Authorization";

  /**
   * Set to true to include Access-Control-Allow-Credentials: true.
   * Must be used together with a concrete (non-wildcard) allow_origin.
   */
  bool        allow_credentials = false;

  /** Max-Age for preflight cache in seconds (default 24 h). */
  int         max_age = 86400;
};

/**
 * @brief Return a CORS middleware with the given configuration.
 *
 * Register via app.use(qbuem::middleware::cors(...)).
 *
 * The middleware:
 *   1. Adds CORS headers to every response (single origin or whitelist match).
 *   2. Short-circuits OPTIONS (preflight) with 204 and stops the chain.
 */
inline Middleware cors(CorsConfig cfg = {}) {
  // Pre-build a hash set for O(1) whitelist lookup.
  std::unordered_set<std::string> whitelist(cfg.allow_origins.begin(),
                                             cfg.allow_origins.end());
  return [cfg = std::move(cfg),
          wl  = std::move(whitelist)](const Request &req, Response &res) -> bool {
    std::string effective_origin = cfg.allow_origin;

    // Dynamic whitelist: reflect the request Origin only if whitelisted.
    if (!wl.empty()) {
      std::string_view req_origin = req.header("Origin");
      if (!req_origin.empty() && wl.count(std::string(req_origin))) {
        effective_origin = std::string(req_origin);
        res.header("Vary", "Origin"); // instruct caches to vary on Origin
      } else {
        // Origin not in whitelist — do not add CORS headers, let chain continue.
        return true;
      }
    }

    res.header("Access-Control-Allow-Origin",  effective_origin);
    res.header("Access-Control-Allow-Methods", cfg.allow_methods);
    res.header("Access-Control-Allow-Headers", cfg.allow_headers);

    if (cfg.allow_credentials)
      res.header("Access-Control-Allow-Credentials", "true");

    if (req.method() == Method::Options) {
      // Preflight response: 204 No Content, no body.
      res.status(204)
         .header("Access-Control-Max-Age", std::to_string(cfg.max_age));
      return false; // halt chain — response sent by the framework
    }

    return true; // continue to next middleware / route handler
  };
}

} // namespace qbuem::middleware
