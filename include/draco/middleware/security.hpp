#pragma once

#include <draco/http/router.hpp>

#include <string>
#include <string_view>

namespace draco::middleware {

/**
 * Strict-Transport-Security (HSTS) middleware.
 *
 * Injects the Strict-Transport-Security header on every response.
 * Only enable this on HTTPS deployments — sending HSTS over plain HTTP is
 * harmless but pointless.
 *
 * @param max_age           Cache duration in seconds (default: 1 year).
 * @param include_subdomains Append "; includeSubDomains" directive.
 * @param preload           Append "; preload" for browser preload list.
 */
inline Middleware hsts(int  max_age           = 31'536'000,
                       bool include_subdomains = true,
                       bool preload           = false) {
  std::string v = "max-age=" + std::to_string(max_age);
  if (include_subdomains) v += "; includeSubDomains";
  if (preload)            v += "; preload";
  return [v = std::move(v)](const Request &, Response &res) -> bool {
    res.header("Strict-Transport-Security", v);
    return true;
  };
}

/**
 * Content-Security-Policy middleware.
 *
 * Example:
 *   app.use(draco::middleware::csp("default-src 'self'; img-src *"));
 */
inline Middleware csp(std::string_view policy) {
  return [p = std::string(policy)](const Request &, Response &res) -> bool {
    res.header("Content-Security-Policy", p);
    return true;
  };
}

/**
 * X-Frame-Options middleware.
 *
 * Prevents the page from being framed by other origins.
 * @param option  "DENY" | "SAMEORIGIN" | "ALLOW-FROM <uri>" (default: SAMEORIGIN)
 */
inline Middleware x_frame_options(std::string_view option = "SAMEORIGIN") {
  return [o = std::string(option)](const Request &, Response &res) -> bool {
    res.header("X-Frame-Options", o);
    return true;
  };
}

/**
 * X-Content-Type-Options: nosniff
 *
 * Prevents browsers from MIME-sniffing the response away from the declared
 * Content-Type.
 */
inline Middleware x_content_type_options() {
  return [](const Request &, Response &res) -> bool {
    res.header("X-Content-Type-Options", "nosniff");
    return true;
  };
}

/**
 * Referrer-Policy middleware.
 *
 * Controls how much referrer information is included in requests.
 * @param policy  Referrer-Policy value (default: strict-origin-when-cross-origin)
 */
inline Middleware referrer_policy(
    std::string_view policy = "strict-origin-when-cross-origin") {
  return [p = std::string(policy)](const Request &, Response &res) -> bool {
    res.header("Referrer-Policy", p);
    return true;
  };
}

/**
 * Permissions-Policy middleware.
 *
 * Restricts browser feature access (camera, microphone, geolocation, etc.).
 * Example policy: "camera=(), microphone=(), geolocation=()"
 */
inline Middleware permissions_policy(std::string_view policy) {
  return [p = std::string(policy)](const Request &, Response &res) -> bool {
    res.header("Permissions-Policy", p);
    return true;
  };
}

/** Configuration for secure_headers(). */
struct SecureHeadersConfig {
  bool        hsts_enabled    = true;
  int         hsts_max_age    = 31'536'000;
  bool        hsts_subdomains = true;
  bool        hsts_preload    = false;
  std::string csp_policy      = "default-src 'self'";
  std::string frame_options   = "SAMEORIGIN";
  std::string referrer        = "strict-origin-when-cross-origin";
  std::string perms_policy    = "";   ///< Empty → header omitted.
};

/**
 * Convenience middleware that applies a standard bundle of security headers in
 * a single call:
 *
 *   - Strict-Transport-Security
 *   - Content-Security-Policy
 *   - X-Frame-Options
 *   - X-Content-Type-Options: nosniff
 *   - Referrer-Policy
 *   - Permissions-Policy (if perms_policy is non-empty)
 *
 * Example:
 *   app.use(draco::middleware::secure_headers());
 */
inline Middleware secure_headers(SecureHeadersConfig cfg = {}) {
  std::string hsts_val;
  if (cfg.hsts_enabled) {
    hsts_val = "max-age=" + std::to_string(cfg.hsts_max_age);
    if (cfg.hsts_subdomains) hsts_val += "; includeSubDomains";
    if (cfg.hsts_preload)    hsts_val += "; preload";
  }

  return [cfg   = std::move(cfg),
          hsts  = std::move(hsts_val)](const Request &, Response &res) -> bool {
    if (!hsts.empty())
      res.header("Strict-Transport-Security", hsts);
    if (!cfg.csp_policy.empty())
      res.header("Content-Security-Policy", cfg.csp_policy);
    if (!cfg.frame_options.empty())
      res.header("X-Frame-Options", cfg.frame_options);
    res.header("X-Content-Type-Options", "nosniff");
    if (!cfg.referrer.empty())
      res.header("Referrer-Policy", cfg.referrer);
    if (!cfg.perms_policy.empty())
      res.header("Permissions-Policy", cfg.perms_policy);
    return true;
  };
}

} // namespace draco::middleware
