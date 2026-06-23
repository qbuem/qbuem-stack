/**
 * @file examples/12-saas/quota/quota_example.cpp
 * @brief How to apply a per-tenant request quota (SaaS multi-tenancy).
 *
 * The `quota()` middleware enforces a fixed-window request budget per tenant
 * (distinct from `rate_limit`, which is a token-bucket rate). It keys on the
 * authenticated tenant (`X-Auth-Sub` set by bearer_auth, then `X-Tenant-Id`),
 * supports per-tenant plan-tier overrides, and emits `X-Quota-*` headers.
 *
 * In a real service:
 *   app.use(qbuem::middleware::bearer_auth(verifier));   // sets X-Auth-Sub
 *   app.use(qbuem::middleware::quota({.limit = 1000, .window = std::chrono::hours{24}}));
 *
 * This demo drives the middleware directly (no server) so it is self-contained.
 */

#include <qbuem/http/parser.hpp>
#include <qbuem/qbuem_stack.hpp>

#include <qbuem/compat/print.hpp> // std::print/println shim (GCC 13 lacks <print>)

#include <chrono>
#include <memory>
#include <string>

using namespace qbuem;
using namespace std::chrono_literals;

namespace {
// Build a parsed GET request carrying a tenant id (zero-copy: keep buf alive).
struct Req {
  std::unique_ptr<std::string> buf;
  Request req;
};
Req make_req(const std::string& tenant) {
  Req r;
  r.buf = std::make_unique<std::string>(
      "GET /api HTTP/1.1\r\nHost: x\r\nX-Tenant-Id: " + tenant + "\r\n\r\n");
  HttpParser{}.parse(*r.buf, r.req);
  return r;
}
} // namespace

int main() {
  std::println("=== Per-tenant quota (limit=3 / window) ===\n");

  // Free tier: 3 requests/window; "vip" tenant gets a higher plan-tier limit.
  auto mw = middleware::quota({
      .limit  = 3,
      .window = 1h,
      .per_key_override =
          [](const std::string& tenant) -> std::optional<long> {
        if (tenant == "vip") return 100;
        return std::nullopt;
      },
  });

  auto acme = make_req("acme");
  std::println("Tenant 'acme' (limit 3):");
  for (int i = 1; i <= 5; ++i) {
    Response res;
    bool ok = mw(acme.req, res);
    std::println("  req {}: {} (status {}, X-Quota-Remaining={})", i,
                 ok ? "allowed" : "BLOCKED", res.status_code(),
                 res.get_header("X-Quota-Remaining"));
  }

  std::println("\nTenant 'vip' (plan override 100):");
  auto vip = make_req("vip");
  int allowed = 0;
  for (int i = 0; i < 10; ++i) {
    Response res;
    if (mw(vip.req, res)) ++allowed;
  }
  std::println("  10 requests → {} allowed (independent, higher budget)", allowed);

  std::println("\nNote: counters are per reactor thread (lock-free); effective");
  std::println("budget = limit x N threads. For an exact global quota, plug a");
  std::println("shared store behind the BYO-DB port.");
  return 0;
}
