/**
 * @file tests/quota_test.cpp
 * @brief Tests for the per-tenant fixed-window quota middleware.
 */

#include <qbuem/http/parser.hpp>
#include <qbuem/middleware/quota.hpp>

#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>

using namespace qbuem;
using namespace qbuem::middleware;
using namespace std::chrono_literals;

namespace {

// Request holds string_views into a receive buffer (zero-copy); the buffer must
// outlive the Request, so it's heap-stable behind a unique_ptr.
struct TestReq {
  std::unique_ptr<std::string> raw;
  Request req;
};

TestReq make_get(const char* path, const char* tenant) {
  TestReq tr;
  tr.raw = std::make_unique<std::string>(
      std::string("GET ") + path + " HTTP/1.1\r\nHost: x\r\n");
  if (tenant) *tr.raw += std::string("X-Tenant-Id: ") + tenant + "\r\n";
  *tr.raw += "\r\n";
  HttpParser parser;
  parser.parse(*tr.raw, tr.req);
  return tr;
}

} // namespace

TEST(QuotaConfig, Defaults) {
  QuotaConfig cfg;
  EXPECT_EQ(cfg.limit, 1000);
  EXPECT_EQ(cfg.window, std::chrono::seconds{3600});
  EXPECT_EQ(cfg.max_keys, 10'000u);
}

TEST(QuotaMiddleware, AllowsUnderLimitThenBlocks) {
  auto mw = quota({.limit = 3, .window = 3600s});
  auto tr = make_get("/api", "acme");

  for (int i = 0; i < 3; ++i) {
    Response res;
    EXPECT_TRUE(mw(tr.req, res)) << "request " << i;
    EXPECT_NE(res.status_code(), 429);
    EXPECT_EQ(res.get_header("X-Quota-Limit"), "3");
  }
  // 4th request exceeds the budget.
  Response res;
  EXPECT_FALSE(mw(tr.req, res));
  EXPECT_EQ(res.status_code(), 429);
  EXPECT_EQ(res.get_header("X-Quota-Remaining"), "0");
  EXPECT_FALSE(res.get_header("Retry-After").empty());
}

TEST(QuotaMiddleware, RemainingCountsDown) {
  auto mw = quota({.limit = 5, .window = 3600s});
  auto tr = make_get("/api", "tenant-r");
  Response r1; mw(tr.req, r1);
  EXPECT_EQ(r1.get_header("X-Quota-Remaining"), "4");
  Response r2; mw(tr.req, r2);
  EXPECT_EQ(r2.get_header("X-Quota-Remaining"), "3");
}

TEST(QuotaMiddleware, PerTenantIsolation) {
  auto mw = quota({.limit = 2, .window = 3600s});
  auto a = make_get("/api", "tenant-a");
  auto b = make_get("/api", "tenant-b");

  // Exhaust tenant-a.
  Response x; EXPECT_TRUE(mw(a.req, x));
  Response y; EXPECT_TRUE(mw(a.req, y));
  Response z; EXPECT_FALSE(mw(a.req, z)); // a is now blocked

  // tenant-b has its own independent budget.
  Response b1; EXPECT_TRUE(mw(b.req, b1));
  EXPECT_NE(b1.status_code(), 429);
}

TEST(QuotaMiddleware, PerKeyOverrideRaisesLimit) {
  QuotaConfig cfg;
  cfg.limit = 1;
  cfg.window = 3600s;
  cfg.per_key_override = [](const std::string& key) -> std::optional<long> {
    if (key == "vip") return 100;
    return std::nullopt;
  };
  auto mw = quota(std::move(cfg));

  auto vip = make_get("/api", "vip");
  for (int i = 0; i < 10; ++i) {
    Response res;
    EXPECT_TRUE(mw(vip.req, res)) << "vip request " << i;
  }
  // A normal tenant still gets the default limit of 1.
  auto reg = make_get("/api", "regular");
  Response ok;  EXPECT_TRUE(mw(reg.req, ok));
  Response no;  EXPECT_FALSE(mw(reg.req, no));
}

TEST(QuotaMiddleware, WindowResets) {
  auto mw = quota({.limit = 1, .window = 1s});
  auto tr = make_get("/api", "tenant-w");

  Response r1; EXPECT_TRUE(mw(tr.req, r1));
  Response r2; EXPECT_FALSE(mw(tr.req, r2)); // exhausted within the window

  std::this_thread::sleep_for(1100ms); // window elapses

  Response r3; EXPECT_TRUE(mw(tr.req, r3)); // budget refreshed
  EXPECT_NE(r3.status_code(), 429);
}
