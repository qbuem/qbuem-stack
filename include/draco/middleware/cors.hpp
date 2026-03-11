#pragma once

/**
 * @file cors.hpp
 * @brief CORS (Cross-Origin Resource Sharing) middleware for Draco WAS.
 *
 * Usage:
 *
 *   #include <draco/middleware/cors.hpp>
 *
 *   // Allow all origins (development / public API)
 *   app.use(draco::middleware::cors());
 *
 *   // Restrict to specific origin
 *   app.use(draco::middleware::cors({
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

#include <draco/http/request.hpp>
#include <draco/http/response.hpp>
#include <draco/http/router.hpp>

#include <string>

namespace draco::middleware {

/** @brief Configuration for the CORS middleware. */
struct CorsConfig {
  /** Value of Access-Control-Allow-Origin. Use "*" for public APIs. */
  std::string allow_origin  = "*";

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
 * Register via app.use(draco::middleware::cors(...)).
 *
 * The middleware:
 *   1. Adds CORS headers to every response.
 *   2. Short-circuits OPTIONS (preflight) with 204 and stops the chain.
 */
inline Middleware cors(CorsConfig cfg = {}) {
  return [cfg = std::move(cfg)](const Request &req, Response &res) -> bool {
    res.header("Access-Control-Allow-Origin",  cfg.allow_origin);
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

} // namespace draco::middleware
