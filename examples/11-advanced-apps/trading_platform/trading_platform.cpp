/**
 * @file examples/trading_platform.cpp
 * @brief qbuem-stack comprehensive platform example — real-time order processing trading server
 *
 * ## System Architecture
 *
 *   [HTTP Client]
 *       │ POST /api/v1/orders  {account_id, symbol, price, quantity, type, side}
 *       ▼
 *   [App Middleware Chain]
 *       ├─ CORS          (allow_origin=*)
 *       ├─ RateLimit     (100 req/s, burst=20)
 *       ├─ RequestID     (X-Request-ID header)
 *       ├─ HSTS          (max_age=31536000)
 *       └─ BearerAuth    (DemoKeyVerifier: "demo-key" → user:alice)
 *       │
 *       ▼ JSON parse → OrderCmd + ResponseChannel Context
 *
 *   ┌── [StaticPipeline: Action Chain] ────────────────────────────────────┐
 *   │   Action<OrderCmd, ValidatedOrder>   validate  {min=1, max=3, auto}  │
 *   │   Action<ValidatedOrder, EnrichedOrder> enrich  {min=2, max=4, auto} │
 *   │   Action<EnrichedOrder, RiskResult>  risk_check {min=1, max=6, slo}  │
 *   └──────────────────────────────────────────────────────────────────────┘
 *       │ ContextualItem<RiskResult> (context-preserving bridge coroutine)
 *       ▼
 *   ┌── [DynamicPipeline<RiskResult>] ─────────────────────────────────────┐
 *   │   Stage "persist"  (MockDB save, order_id generation) ← hot_swap     │
 *   │   Stage "enrich2"  (additional calculation, set_enabled capable)     │
 *   │   Stage "notify"   (ResponseChannel write + MessageBus publish)      │
 *   └──────────────────────────────────────────────────────────────────────┘
 *       │
 *       ├─ ctx.get<ResponseChannel>() → resp_ch.try_send(OrderResult)
 *       │   ↑
 *       │   └── HTTP handler co_await resp_ch->recv() → return JSON response
 *       │
 *       └─ MessageBus.publish("order.completed") → fan-out
 *           ├─ GET /api/v1/events  (SSE stream, real-time events)
 *           └─ Stats counter update
 *
 *   [Auto-scaler coroutine]
 *       └─ polls validate/enrich/risk input queue depth every 500ms
 *           → queue > 50 → scale_out(dispatcher)
 *           → queue <  5 → scale_in()
 *
 * ## API Endpoints
 *   POST   /api/v1/orders              submit order (respond after pipeline processing)
 *   GET    /api/v1/orders              list all orders (mock DB)
 *   GET    /api/v1/orders/:id          get order
 *   DELETE /api/v1/orders/:id          cancel order
 *   GET    /api/v1/events              SSE real-time event stream
 *   GET    /api/v1/stats               pipeline status / worker count / queue depth
 *   POST   /api/v1/admin/scale         manual scale adjustment
 *   POST   /api/v1/admin/hotswap       DynamicPipeline stage replacement
 *   POST   /api/v1/admin/toggle        DynamicPipeline stage enable/disable
 *   GET    /health                     health check
 */

// ─── Includes ─────────────────────────────────────────────────────────────────

#include <qbuem/core/awaiters.hpp>

#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/http/request.hpp>
#include <qbuem/http/response.hpp>
#include <qbuem/http/router.hpp>
#include <qbuem/middleware/cors.hpp>
#include <qbuem/middleware/rate_limit.hpp>
#include <qbuem/middleware/request_id.hpp>
#include <qbuem/middleware/security.hpp>
#include <qbuem/middleware/sse.hpp>
#include <qbuem/middleware/token_auth.hpp>
#include <qbuem/pipeline/action.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/async_channel.hpp>
#include <qbuem/pipeline/context.hpp>
#include <qbuem/pipeline/dynamic_pipeline.hpp>
#include <qbuem/pipeline/message_bus.hpp>
#include <qbuem/pipeline/slo.hpp>
#include <qbuem/pipeline/static_pipeline.hpp>
#include <qbuem/qbuem_stack.hpp>
#include <qbuem_json/qbuem_json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <qbuem/compat/print.hpp>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <algorithm>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace qbuem;
using namespace qbuem::middleware;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// §2. Domain Types + HTTP DTO (qbuem-json Nexus engine serialization/deserialization)
// ─────────────────────────────────────────────────────────────────────────────

/// Incoming HTTP order command (Pipeline input type).
struct OrderCmd {
    uint64_t    req_id    = 0;       ///< unique HTTP request ID (also propagated via Context)
    std::string account_id;
    std::string symbol;
    double      price     = 0.0;
    int         quantity  = 0;
    bool        is_market = false;   ///< true=market order, false=limit order
    bool        is_buy    = true;    ///< true=buy, false=sell

    /// Deserialize from HTTP body (JSON) — uses Nexus qbuem::fuse<OrderCmd>().
    /// Returns std::nullopt if required fields are missing.
    static std::optional<OrderCmd> from_json(std::string_view body) {
        try {
            auto cmd = qbuem::fuse<OrderCmd>(std::string(body));
            if (cmd.account_id.empty() || cmd.symbol.empty() || cmd.quantity == 0)
                return std::nullopt;
            return cmd;
        } catch (...) { return std::nullopt; }
    }
};

/// Nexus Fusion ADL hook — maps JSON "type"/"side" keys to is_market/is_buy.
/// Used when calling qbuem::fuse<OrderCmd>() (zero-tape direct parsing).
inline void nexus_pulse(std::string_view key, const char*& p, const char* end, OrderCmd& o) {
    using namespace qbuem::json::detail;
    switch (fnv1a_hash(key)) {
        case fnv1a_hash_ce("account_id"): from_json_direct(p, end, o.account_id); break;
        case fnv1a_hash_ce("symbol"):     from_json_direct(p, end, o.symbol);     break;
        case fnv1a_hash_ce("price"):      from_json_direct(p, end, o.price);      break;
        case fnv1a_hash_ce("quantity"):   from_json_direct(p, end, o.quantity);   break;
        case fnv1a_hash_ce("type"): {
            std::string s; from_json_direct(p, end, s);
            o.is_market = (s == "market");
            break;
        }
        case fnv1a_hash_ce("side"): {
            std::string s; from_json_direct(p, end, s);
            o.is_buy = (s == "buy");
            break;
        }
        default: skip_direct(p, end); break;
    }
}

/// [Static Stage 1] Validation result.
struct ValidatedOrder {
    OrderCmd    cmd;
    bool        valid     = true;
    std::string error_msg;
    double      notional  = 0.0;     ///< price × quantity
};

/// [Static Stage 2] Account/market data enrichment result.
struct EnrichedOrder {
    ValidatedOrder validated;
    // populated only when valid:
    std::string    account_name;
    double         account_balance = 0.0;
    double         market_price    = 0.0;   ///< current market price
    double         slippage_bps    = 0.0;   ///< estimated slippage (basis points)
};

/// [Static Stage 3] Risk evaluation result (Static Pipeline final output).
struct RiskResult {
    EnrichedOrder enriched;
    bool          risk_ok     = false;
    std::string   risk_reason;
    double        var_1d      = 0.0;     ///< 1-day VaR estimate (KRW)
};

/// Mock DB storage record.
struct OrderRecord {
    uint64_t    order_id    = 0;
    std::string account_id;
    std::string symbol;
    double      price       = 0.0;
    int         quantity    = 0;
    std::string side;                    ///< "buy" | "sell"
    std::string type;                    ///< "market" | "limit"
    std::string status;                  ///< "filled" | "rejected" | "cancelled"
    double      exec_price  = 0.0;
    std::string created_at;
    std::string reason;                  ///< rejection/cancellation reason
};
QBUEM_JSON_FIELDS(OrderRecord, order_id, account_id, symbol, price, quantity,
                  side, type, status, exec_price, created_at, reason)

/// HTTP response DTO (Pipeline → HTTP handler).
struct OrderResult {
    uint64_t    order_id   = 0;
    std::string status;
    double      exec_price = 0.0;
    std::string message;
    bool        success    = false;
};
QBUEM_JSON_FIELDS(OrderResult, order_id, status, exec_price, message, success)

/// MessageBus event (for SSE streaming).
struct OrderEvent {
    uint64_t    order_id   = 0;
    std::string account_id;
    std::string symbol;
    std::string status;
    double      exec_price = 0.0;
    std::string reason;
};
QBUEM_JSON_FIELDS(OrderEvent, order_id, account_id, symbol, status, exec_price, reason)

// ─────────────────────────────────────────────────────────────────────────────
// §3. Context Tags — metadata that passes through all pipeline layers
// ─────────────────────────────────────────────────────────────────────────────

/// HTTP response channel: the last Pipeline Stage writes the result here.
struct ResponseChannel {
    std::shared_ptr<AsyncChannel<OrderResult>> ch;
};

// RequestId, AuthSubject: use built-in types already defined in context.hpp

// ── Admin Request DTOs ───────────────────────────────────────────────────────

/// POST /api/v1/admin/scale request.
struct ScaleRequest {
    std::string stage;
    int         workers = 0;

    static std::optional<ScaleRequest> from_json(std::string_view body) {
        try {
            auto r = qbuem::fuse<ScaleRequest>(std::string(body));
            if (r.stage.empty() || r.workers <= 0) return std::nullopt;
            return r;
        } catch (...) { return std::nullopt; }
    }
};
QBUEM_JSON_FIELDS(ScaleRequest, stage, workers)

/// POST /api/v1/admin/hotswap request.
struct HotswapRequest {
    std::string stage;
    std::string mode;

    static std::optional<HotswapRequest> from_json(std::string_view body) {
        try {
            auto r = qbuem::fuse<HotswapRequest>(std::string(body));
            if (r.stage.empty() || r.mode.empty()) return std::nullopt;
            return r;
        } catch (...) { return std::nullopt; }
    }
};
QBUEM_JSON_FIELDS(HotswapRequest, stage, mode)

/// POST /api/v1/admin/toggle request.
/// Body: {"stage": "enrich2", "enabled": true|false}
struct ToggleRequest {
    std::string stage;
    bool        enabled = false;

    static std::optional<ToggleRequest> from_json(std::string_view body) {
        try {
            auto r = qbuem::fuse<ToggleRequest>(std::string(body));
            if (r.stage.empty()) return std::nullopt;
            return r;
        } catch (...) { return std::nullopt; }
    }
};
QBUEM_JSON_FIELDS(ToggleRequest, stage, enabled)

// ── HTTP Response DTOs ───────────────────────────────────────────────────────

/// Error response.
struct ErrorResponse {
    std::string error;
};
QBUEM_JSON_FIELDS(ErrorResponse, error)

/// DELETE /api/v1/orders/:id response.
struct CancelResponse {
    uint64_t    order_id = 0;
    std::string status;
};
QBUEM_JSON_FIELDS(CancelResponse, order_id, status)

/// SSE connection confirmation event.
struct SseConnectedEvent {
    std::string message;
    std::string topic;
};
QBUEM_JSON_FIELDS(SseConnectedEvent, message, topic)

/// POST /api/v1/admin/scale response.
struct ScaleResponse {
    std::string stage;
    int         workers = 0;
    std::string message;
};
QBUEM_JSON_FIELDS(ScaleResponse, stage, workers, message)

/// POST /api/v1/admin/hotswap response.
struct HotswapResponse {
    std::string stage;
    std::string mode;
    bool        success = false;
};
QBUEM_JSON_FIELDS(HotswapResponse, stage, mode, success)

/// POST /api/v1/admin/toggle response.
struct ToggleResponse {
    std::string stage;
    bool        enabled = false;
    bool        success = false;
};
QBUEM_JSON_FIELDS(ToggleResponse, stage, enabled, success)

/// GET /api/v1/orders response (list).
struct OrderListResponse {
    std::vector<OrderRecord> orders;

    /// Nexus: OrderRecord has QBUEM_JSON_FIELDS so the vector is serialized directly.
    std::string to_json() const { return qbuem::write(orders); }
};

/// GET /api/v1/stats response.
struct StatsResponse {
    uint64_t                 submitted      = 0;
    uint64_t                 filled         = 0;
    uint64_t                 rejected       = 0;
    uint64_t                 db_total       = 0;
    bool                     validate_empty = false;
    bool                     enrich_empty   = false;
    bool                     risk_empty     = false;
    int                      dyn_stages     = 0;
    std::vector<std::string> dyn_stage_names;
    bool                     auto_scale     = false;
};
QBUEM_JSON_FIELDS(StatsResponse, submitted, filled, rejected, db_total,
                  validate_empty, enrich_empty, risk_empty, dyn_stages,
                  dyn_stage_names, auto_scale)

// ─────────────────────────────────────────────────────────────────────────────
// §4. Mock DB (thread-safe in-memory storage)
// ─────────────────────────────────────────────────────────────────────────────

class MockDB {
public:
    uint64_t save(OrderRecord rec) {
        std::lock_guard lock(mtx_);
        rec.order_id = ++next_id_;
        rec.created_at = now_str();
        orders_[rec.order_id] = rec;
        return rec.order_id;
    }

    std::optional<OrderRecord> get(uint64_t id) const {
        std::lock_guard lock(mtx_);
        auto it = orders_.find(id);
        if (it == orders_.end()) return std::nullopt;
        return it->second;
    }

    bool update_status(uint64_t id, std::string_view status, std::string_view reason = {}) {
        std::lock_guard lock(mtx_);
        auto it = orders_.find(id);
        if (it == orders_.end()) return false;
        it->second.status = status;
        if (!reason.empty()) it->second.reason = reason;
        return true;
    }

    std::vector<OrderRecord> list(size_t limit = 50) const {
        std::lock_guard lock(mtx_);
        std::vector<OrderRecord> result;
        for (auto it = orders_.begin(); it != orders_.end() && result.size() < limit; ++it)
            result.push_back(it->second);
        std::reverse(result.begin(), result.end());
        return result;
    }

    size_t count() const {
        std::lock_guard lock(mtx_);
        return orders_.size();
    }

private:
    static std::string now_str() {
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
        return buf;
    }

    mutable std::mutex                       mtx_;
    std::unordered_map<uint64_t, OrderRecord> orders_;
    uint64_t                                  next_id_{0};
};

// ─────────────────────────────────────────────────────────────────────────────
// §5. Mock Market Data
// ─────────────────────────────────────────────────────────────────────────────

struct MockAccount {
    std::string name;
    double      balance; // KRW
};

static const std::unordered_map<std::string, MockAccount> kAccounts = {
    {"ACC001", {"Alice",   100'000'000.0}},
    {"ACC002", {"Bob",      50'000'000.0}},
    {"ACC003", {"Charlie",  10'000'000.0}},
    {"ACC004", {"Dave",      1'000'000.0}},  // low balance for testing insufficient funds
};

static const std::unordered_map<std::string, double> kMarketPrices = {
    {"SAMSUNG",  72'500.0},
    {"SKHYNIX",  93'000.0},
    {"KAKAO",    55'000.0},
    {"NAVER",   210'000.0},
    {"KRAFTON", 235'000.0},
    {"LGCHEM",  395'000.0},
    {"HYUNDAI", 205'000.0},
    {"POSCO",   382'000.0},
};

// ─────────────────────────────────────────────────────────────────────────────
// §6. Auth Token Verifier (ITokenVerifier implementation)
// ─────────────────────────────────────────────────────────────────────────────

class DemoKeyVerifier : public ITokenVerifier {
public:
    std::optional<TokenClaims> verify(std::string_view token) noexcept override {
        // demo: allow "demo-key", "demo-key-dave" → Dave account
        if (token == "demo-key") {
            return TokenClaims{
                .subject  = "ACC001",
                .issuer   = "trading-platform",
                .audience = "order-api",
                .exp      = 9'999'999'999L,
                .custom   = {{"role", "trader"}},
            };
        }
        if (token == "demo-key-bob") {
            return TokenClaims{.subject="ACC002", .issuer="trading-platform",
                               .audience="order-api", .exp=9'999'999'999L};
        }
        if (token == "demo-key-dave") {
            return TokenClaims{.subject="ACC004", .issuer="trading-platform",
                               .audience="order-api", .exp=9'999'999'999L};
        }
        if (token == "admin-key") {
            return TokenClaims{.subject="admin", .issuer="trading-platform",
                               .audience="order-api", .exp=9'999'999'999L,
                               .custom={{"role","admin"}}};
        }
        return std::nullopt;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// §7. Static Pipeline Stage Functions
// ─────────────────────────────────────────────────────────────────────────────

/// Stage 1: validation (format/range check)
static Task<Result<ValidatedOrder>> stage_validate(OrderCmd cmd, ActionEnv env) {
    ValidatedOrder v;
    v.cmd = cmd;

    if (cmd.symbol.empty()) {
        v.valid     = false;
        v.error_msg = "symbol required";
        co_return v;
    }
    if (kMarketPrices.find(cmd.symbol) == kMarketPrices.end()) {
        v.valid     = false;
        v.error_msg = "unknown symbol: " + cmd.symbol;
        co_return v;
    }
    if (!cmd.is_market && cmd.price <= 0.0) {
        v.valid     = false;
        v.error_msg = "limit order requires price > 0";
        co_return v;
    }
    if (cmd.quantity <= 0) {
        v.valid     = false;
        v.error_msg = "quantity must be > 0";
        co_return v;
    }
    if (cmd.account_id.empty()) {
        v.valid     = false;
        v.error_msg = "account_id required";
        co_return v;
    }

    double price = cmd.is_market ? kMarketPrices.at(cmd.symbol) : cmd.price;
    v.notional = price * cmd.quantity;

    std::println("  [validate #{:02}] worker={} {} {} {} qty={} notional={:.0f}",
        static_cast<unsigned long long>(cmd.req_id),
        env.worker_idx,
        cmd.is_buy ? "BUY" : "SELL",
        cmd.symbol,
        cmd.is_market ? "MARKET" : "LIMIT",
        cmd.quantity,
        v.notional);

    co_return v;
}

/// Stage 2: account/market data enrichment — simulates external data lookup
static Task<Result<EnrichedOrder>> stage_enrich(ValidatedOrder v, ActionEnv env) {
    EnrichedOrder e;
    e.validated = v;

    if (!v.valid) co_return e;  // already failed → pass-through

    auto acct_it = kAccounts.find(v.cmd.account_id);
    if (acct_it == kAccounts.end()) {
        e.validated.valid     = false;
        e.validated.error_msg = "account not found: " + v.cmd.account_id;
        co_return e;
    }

    e.account_name    = acct_it->second.name;
    e.account_balance = acct_it->second.balance;
    e.market_price    = kMarketPrices.at(v.cmd.symbol);

    // slippage calculation (limit order: deviation from market price)
    if (!v.cmd.is_market) {
        e.slippage_bps = std::abs(v.cmd.price - e.market_price) / e.market_price * 10000.0;
    }

    // insufficient balance check (buy side)
    if (v.cmd.is_buy && v.notional > e.account_balance) {
        e.validated.valid     = false;
        e.validated.error_msg = "insufficient balance";
    }

    std::println("  [enrich   #{:02}] worker={} acct={} balance={:.0f} mkt_price={:.0f} slippage={:.1f}bps",
        static_cast<unsigned long long>(v.cmd.req_id),
        env.worker_idx,
        e.account_name,
        e.account_balance,
        e.market_price,
        e.slippage_bps);

    co_return e;
}

/// Stage 3: risk evaluation — VaR estimation, limit check
static Task<Result<RiskResult>> stage_risk_check(EnrichedOrder e, ActionEnv env) {
    RiskResult r;
    r.enriched = e;

    if (!e.validated.valid) {
        r.risk_ok     = false;
        r.risk_reason = e.validated.error_msg;
        co_return r;
    }

    // 1-day VaR estimate: simplified formula (assuming 2% daily volatility)
    r.var_1d = e.validated.notional * 0.02;

    // risk limit check
    constexpr double kMaxNotional = 50'000'000.0;   // 50M KRW
    constexpr double kMaxVaR      =    500'000.0;   // 500K KRW

    if (e.validated.notional > kMaxNotional) {
        r.risk_ok     = false;
        r.risk_reason = "notional exceeds limit (max 50M KRW)";
    } else if (r.var_1d > kMaxVaR) {
        r.risk_ok     = false;
        r.risk_reason = "VaR exceeds limit";
    } else {
        r.risk_ok = true;
    }

    std::println("  [risk     #{:02}] worker={} var1d={:.0f} ok={} {}",
        static_cast<unsigned long long>(e.validated.cmd.req_id),
        env.worker_idx,
        r.var_1d,
        r.risk_ok ? "YES" : "NO",
        r.risk_ok ? "" : r.risk_reason);

    co_return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// §8. Dynamic Pipeline Stage Functions
// ─────────────────────────────────────────────────────────────────────────────

/// Dynamic Stage "persist": MockDB save, order_id generation
/// — hot_swap target (can be replaced with persist_fast version)
static auto make_persist_stage(std::shared_ptr<MockDB> db) {
    return [db](RiskResult risk, ActionEnv env) -> Task<Result<RiskResult>> {
        const auto& cmd = risk.enriched.validated.cmd;

        OrderRecord rec;
        rec.account_id  = cmd.account_id;
        rec.symbol      = cmd.symbol;
        rec.price       = cmd.price;
        rec.quantity    = cmd.quantity;
        rec.side        = cmd.is_buy ? "buy" : "sell";
        rec.type        = cmd.is_market ? "market" : "limit";

        if (!risk.enriched.validated.valid) {
            rec.status = "rejected";
            rec.reason = risk.enriched.validated.error_msg;
        } else if (!risk.risk_ok) {
            rec.status = "rejected";
            rec.reason = risk.risk_reason;
        } else {
            rec.status     = "filled";
            rec.exec_price = risk.enriched.market_price;  // filled at market price
        }

        uint64_t oid = db->save(rec);

        // inject order_id into RiskResult (used by notify stage)
        // instead of storing in Context, simply overwrite cmd.req_id with order_id
        risk.enriched.validated.cmd.req_id = oid;  // reuse field: overwrite with order_id

        std::println("  [persist  #{:02}] worker={} status={} exec_price={:.0f}",
            static_cast<unsigned long long>(oid),
            env.worker_idx,
            rec.status,
            rec.exec_price);

        co_return risk;
    };
}

/// Dynamic Stage "persist_fast": hot_swap replacement target (simplified version)
static auto make_persist_fast_stage(std::shared_ptr<MockDB> db) {
    return [db](RiskResult risk, ActionEnv env) -> Task<Result<RiskResult>> {
        const auto& cmd = risk.enriched.validated.cmd;
        OrderRecord rec;
        rec.account_id  = cmd.account_id;
        rec.symbol      = cmd.symbol;
        rec.price       = cmd.price;
        rec.quantity    = cmd.quantity;
        rec.side        = cmd.is_buy ? "buy" : "sell";
        rec.type        = cmd.is_market ? "market" : "limit";
        rec.status      = (risk.enriched.validated.valid && risk.risk_ok) ? "filled" : "rejected";
        rec.exec_price  = risk.enriched.market_price;
        rec.reason      = risk.risk_ok ? "" : risk.risk_reason;

        uint64_t oid = db->save(rec);
        risk.enriched.validated.cmd.req_id = oid;

        std::println("  [persist★ #{:02}] worker={} [FAST MODE] status={}",
            static_cast<unsigned long long>(oid), env.worker_idx, rec.status);

        co_return risk;
    };
}

/// Dynamic Stage "enrich2": additional calculation (demonstrates set_enabled — pass-through when disabled)
static Task<Result<RiskResult>> stage_enrich2(RiskResult risk, ActionEnv env) {
    // additional slippage cost calculation (optional stage)
    if (risk.enriched.validated.valid) {
        double slippage_cost = risk.enriched.validated.notional
                               * risk.enriched.slippage_bps / 10000.0;
        std::println("  [enrich2  #{:02}] worker={} slippage_cost={:.2f} KRW",
            static_cast<unsigned long long>(risk.enriched.validated.cmd.req_id),
            env.worker_idx, slippage_cost);
    }
    co_return risk;
}

/// Dynamic Stage "notify": write result to ResponseChannel + publish to MessageBus
static auto make_notify_stage(std::shared_ptr<MessageBus> bus,
                               std::atomic<uint64_t>& filled,
                               std::atomic<uint64_t>& rejected) {
    return [bus, &filled, &rejected](RiskResult risk, ActionEnv env) -> Task<Result<RiskResult>> {
        uint64_t oid = risk.enriched.validated.cmd.req_id;  // set by persist stage
        bool success = risk.enriched.validated.valid && risk.risk_ok;

        // ── 1) send HTTP response data to ResponseChannel ───────────────────
        if (auto* rch = env.ctx.get_ptr<ResponseChannel>()) {
            OrderResult result;
            result.order_id   = oid;
            result.status     = success ? "filled" : "rejected";
            result.exec_price = success ? risk.enriched.market_price : 0.0;
            result.message    = success ? "Order executed successfully"
                                        : (risk.enriched.validated.valid
                                            ? risk.risk_reason
                                            : risk.enriched.validated.error_msg);
            result.success    = success;
            rch->ch->try_send(result);  // wakes up HTTP handler's co_await recv()
        }

        // ── 2) update stats ────────────────────────────────────────────────
        if (success)
            filled.fetch_add(1, std::memory_order_relaxed);
        else
            rejected.fetch_add(1, std::memory_order_relaxed);

        // ── 3) MessageBus publish (SSE fan-out) ───────────────────────────
        OrderEvent event{
            .order_id   = oid,
            .account_id = risk.enriched.validated.cmd.account_id,
            .symbol     = risk.enriched.validated.cmd.symbol,
            .status     = success ? "filled" : "rejected",
            .exec_price = success ? risk.enriched.market_price : 0.0,
            .reason     = success ? "" : (risk.enriched.validated.valid
                                          ? risk.risk_reason
                                          : risk.enriched.validated.error_msg),
        };
        bus->try_publish("order.completed", event);

        std::println("  [notify   #{:02}] worker={} → HTTP response sent, event published",
            static_cast<unsigned long long>(oid), env.worker_idx);

        co_return risk;
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// §9. TradingPlatform — struct holding all state
// ─────────────────────────────────────────────────────────────────────────────

struct TradingPlatform {
    // ── Infrastructure ────────────────────────────────────────────────────
    std::shared_ptr<MockDB>     db  = std::make_shared<MockDB>();
    std::shared_ptr<MessageBus> bus = std::make_shared<MessageBus>();

    // ── Static Pipeline Actions (held directly for scale control) ─────────
    // type aliases
    using VAction = Action<OrderCmd, ValidatedOrder>;
    using EAction = Action<ValidatedOrder, EnrichedOrder>;
    using RAction = Action<EnrichedOrder, RiskResult>;

    std::shared_ptr<VAction> validate_act;
    std::shared_ptr<EAction> enrich_act;
    std::shared_ptr<RAction> risk_act;

    // final output channel of Static Pipeline (read by bridge coroutine)
    std::shared_ptr<AsyncChannel<ContextualItem<RiskResult>>> risk_out;

    // ── Dynamic Pipeline ──────────────────────────────────────────────────
    std::shared_ptr<DynamicPipeline<RiskResult>> dyn_pipe;

    // ── Stats ─────────────────────────────────────────────────────────────
    std::atomic<uint64_t> submitted{0};
    std::atomic<uint64_t> filled{0};
    std::atomic<uint64_t> rejected{0};
    std::atomic<bool>     auto_scale_on{true};
    std::atomic<uint64_t> req_counter{0};

    // ── Dispatcher ref (needed for scale_out calls) ───────────────────────
    Dispatcher* disp = nullptr;

    /// Start all pipelines and buses.
    void setup(Dispatcher& d) {
        disp = &d;
        bus->start(d);

        // ── Create Static Pipeline Actions ───────────────────────────────

        validate_act = std::make_shared<VAction>(
            stage_validate,
            VAction::Config{
                .min_workers = 1,
                .max_workers = 3,
                .channel_cap = 512,
                .auto_scale  = true,
                .slo = SloConfig{
                    .p99_target  = std::chrono::microseconds{5'000},
                    .error_budget = 0.001,
                },
            });

        enrich_act = std::make_shared<EAction>(
            stage_enrich,
            EAction::Config{
                .min_workers = 2,
                .max_workers = 4,
                .channel_cap = 256,
                .auto_scale  = true,
            });

        risk_act = std::make_shared<RAction>(
            stage_risk_check,
            RAction::Config{
                .min_workers = 1,
                .max_workers = 6,
                .channel_cap = 256,
                .auto_scale  = true,
                .slo = SloConfig{
                    .p99_target  = std::chrono::microseconds{50'000},
                    .error_budget = 0.005,
                },
            });

        // ── Connect and start Static Actions manually ────────────────────
        // startup order: last Action first (output channel must exist)
        risk_out = std::make_shared<AsyncChannel<ContextualItem<RiskResult>>>(512);
        risk_act->start(d, risk_out);                        // risk → risk_out
        enrich_act->start(d, risk_act->input());             // enrich → risk input
        validate_act->start(d, enrich_act->input());         // validate → enrich input

        // ── Create Dynamic Pipeline ───────────────────────────────────────
        dyn_pipe = std::make_shared<DynamicPipeline<RiskResult>>(
            DynamicPipeline<RiskResult>::Config{
                .default_channel_cap = 256,
                .default_workers     = 2,
            });

        dyn_pipe->add_stage("persist",  make_persist_stage(db));
        dyn_pipe->add_stage("enrich2",  stage_enrich2);
        dyn_pipe->add_stage("notify",   make_notify_stage(bus, filled, rejected));

        dyn_pipe->start(d);

        // ── Static → Dynamic bridge coroutine (preserves Context) ────────
        // NOTE: uses static member function instead of coroutine lambda
        //       (workaround for GCC 13 coroutine lambda lifetime bug)
        d.spawn(run_bridge(risk_out, dyn_pipe));

        // ── Auto-scaler coroutine ─────────────────────────────────────────
        d.spawn(run_autoscaler(this));

        std::println("[platform] setup complete: static(3 actions) + dynamic(3 stages) + autoscaler");
    }

    // ── Static Member Coroutines (workaround for GCC 13 lambda lifetime bug) ────

    /// Static → Dynamic bridge: connect both pipelines while preserving Context
    static Task<void> run_bridge(
        std::shared_ptr<AsyncChannel<ContextualItem<RiskResult>>> risk_out,
        std::shared_ptr<DynamicPipeline<RiskResult>> dyn_pipe)
    {
        for (;;) {
            auto item = co_await risk_out->recv();
            if (!item) break;
            co_await dyn_pipe->push(item->value, item->ctx);
        }
    }

    /// Auto-scaler: scale_out/scale_in based on queue depth
    static Task<void> run_autoscaler(TradingPlatform* p) {
        for (;;) {
            co_await qbuem::sleep(500);

            if (!p->auto_scale_on.load()) continue;

            // detect overload via AsyncChannel size_approx() > 0
            bool v_busy = p->validate_act->input()->size_approx() > 0;
            bool e_busy = p->enrich_act->input()->size_approx() > 0;
            bool r_busy = p->risk_act->input()->size_approx() > 0;

            if (v_busy) {
                p->validate_act->scale_out(*p->disp);
                std::println("[autoscale] validate busy → scale_out");
            } else {
                p->validate_act->scale_in();
            }
            if (e_busy) {
                p->enrich_act->scale_out(*p->disp);
                std::println("[autoscale] enrich busy → scale_out");
            } else {
                p->enrich_act->scale_in();
            }
            if (r_busy) {
                p->risk_act->scale_out(*p->disp);
                std::println("[autoscale] risk busy → scale_out");
            } else {
                p->risk_act->scale_in();
            }
        }
    }

    /// Submit an order to the pipeline.
    /// @returns channel to receive the result (await with co_await recv())
    std::shared_ptr<AsyncChannel<OrderResult>>
    begin_order(OrderCmd cmd, Context base_ctx) {
        auto resp_ch = std::make_shared<AsyncChannel<OrderResult>>(1);
        auto ctx = base_ctx
            .put(ResponseChannel{resp_ch});
        cmd.req_id = ++req_counter;
        // non-blocking try: returns nullptr when saturated (caller handles 503)
        if (!validate_act->try_push(cmd, ctx))
            return nullptr;
        submitted.fetch_add(1, std::memory_order_relaxed);
        return resp_ch;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// §10. HTTP Handlers
// ─────────────────────────────────────────────────────────────────────────────

/// POST /api/v1/orders — submit order (respond after pipeline processing)
static AsyncHandler make_post_order(TradingPlatform& p) {
    return [&p](const Request& req, Response& res) -> Task<void> {
        auto cmd = OrderCmd::from_json(req.body());
        if (!cmd) {
            res.status(400)
               .header("Content-Type", "application/json")
               .body(qbuem::write(ErrorResponse{"required: account_id, symbol, quantity, type, side"}));
            co_return;
        }

        // inject auth info into Context
        auto ctx = Context{}
            .put(RequestId{std::string(req.header("X-Request-ID"))})
            .put(AuthSubject{std::string(req.header("X-Auth-Sub"))});

        // submit to Pipeline (async)
        auto resp_ch = p.begin_order(*cmd, ctx);
        if (!resp_ch) {
            res.status(503)
               .header("Content-Type", "application/json")
               .body(qbuem::write(ErrorResponse{"pipeline overloaded, retry later"}));
            co_return;
        }

        // wait for pipeline processing to complete (cross-reactor safe)
        auto result = co_await resp_ch->recv();
        if (!result) {
            res.status(500)
               .header("Content-Type", "application/json")
               .body(qbuem::write(ErrorResponse{"internal pipeline error"}));
            co_return;
        }

        res.status(result->success ? 201 : 422)
           .header("Content-Type", "application/json")
           .body(qbuem::write(*result));
    };
}

/// GET /api/v1/orders — list all orders
static Handler make_get_orders(TradingPlatform& p) {
    return [&p](const Request& /*req*/, Response& res) {
        res.status(200)
           .header("Content-Type", "application/json")
           .body(OrderListResponse{p.db->list(100)}.to_json());
    };
}

/// GET /api/v1/orders/:id — get single order
static Handler make_get_order(TradingPlatform& p) {
    return [&p](const Request& req, Response& res) {
        uint64_t id = 0;
        try { id = std::stoull(std::string(req.param("id"))); }
        catch (...) {
            res.status(400).body(qbuem::write(ErrorResponse{"invalid order id"}));
            return;
        }
        auto rec = p.db->get(id);
        if (!rec) {
            res.status(404).body(qbuem::write(ErrorResponse{"order not found"}));
            return;
        }
        res.status(200)
           .header("Content-Type", "application/json")
           .body(qbuem::write(*rec));
    };
}

/// DELETE /api/v1/orders/:id — cancel order
static Handler make_cancel_order(TradingPlatform& p) {
    return [&p](const Request& req, Response& res) {
        uint64_t id = 0;
        try { id = std::stoull(std::string(req.param("id"))); } catch (...) {
            res.status(400).body(qbuem::write(ErrorResponse{"invalid order id"})); return;
        }
        auto rec = p.db->get(id);
        if (!rec) { res.status(404).body(qbuem::write(ErrorResponse{"order not found"})); return; }
        if (rec->status != "filled") {
            res.status(409).body(qbuem::write(ErrorResponse{"only filled orders can be cancelled"})); return;
        }
        p.db->update_status(id, "cancelled", "user requested");
        res.status(200)
           .header("Content-Type", "application/json")
           .body(qbuem::write(CancelResponse{id, "cancelled"}));
    };
}

/// GET /api/v1/events — SSE real-time event stream
static AsyncHandler make_sse_events(TradingPlatform& p) {
    return [&p](const Request& /*req*/, Response& res) -> Task<void> {
        // subscribe to MessageBus "order.completed" (streaming)
        auto stream = p.bus->subscribe_stream<OrderEvent>("order.completed", 64);

        SseStream sse(res);
        sse.send(qbuem::write(SseConnectedEvent{"connected", "order.completed"}), "connected");

        // stop after at most 30 events (demo)
        for (int i = 0; i < 30; ++i) {
            auto ev = co_await stream->recv();
            if (!ev) break;
            sse.send(qbuem::write(*ev), "order");
        }
        sse.close();
    };
}

/// GET /api/v1/stats — pipeline status and worker counts
static Handler make_get_stats(TradingPlatform& p) {
    return [&p](const Request& /*req*/, Response& res) {
        StatsResponse s;
        s.submitted       = static_cast<uint64_t>(p.submitted.load());
        s.filled          = static_cast<uint64_t>(p.filled.load());
        s.rejected        = static_cast<uint64_t>(p.rejected.load());
        s.db_total        = static_cast<uint64_t>(p.db->count());
        s.validate_empty  = p.validate_act->input()->size_approx() == 0;
        s.enrich_empty    = p.enrich_act->input()->size_approx() == 0;
        s.risk_empty      = p.risk_act->input()->size_approx() == 0;
        s.dyn_stages      = static_cast<int>(p.dyn_pipe->stage_count());
        s.dyn_stage_names = p.dyn_pipe->stage_names();
        s.auto_scale      = p.auto_scale_on.load();

        res.status(200)
           .header("Content-Type", "application/json")
           .body(qbuem::write(s));
    };
}

/// POST /api/v1/admin/scale — manual scale adjustment
/// Body: {"stage": "validate"|"enrich"|"risk", "workers": N}
static Handler make_post_scale(TradingPlatform& p) {
    return [&p](const Request& req, Response& res) {
        auto req_dto = ScaleRequest::from_json(req.body());
        if (!req_dto) {
            res.status(400).body(qbuem::write(ErrorResponse{"required: stage, workers"})); return;
        }

        if (req_dto->stage == "validate") {
            p.validate_act->scale_to(static_cast<size_t>(req_dto->workers), *p.disp);
        } else if (req_dto->stage == "enrich") {
            p.enrich_act->scale_to(static_cast<size_t>(req_dto->workers), *p.disp);
        } else if (req_dto->stage == "risk") {
            p.risk_act->scale_to(static_cast<size_t>(req_dto->workers), *p.disp);
        } else {
            res.status(400).body(qbuem::write(ErrorResponse{"stage must be: validate|enrich|risk"})); return;
        }

        res.status(200)
           .header("Content-Type", "application/json")
           .body(qbuem::write(ScaleResponse{req_dto->stage, req_dto->workers, "scale applied"}));
    };
}

/// POST /api/v1/admin/hotswap — DynamicPipeline "persist" stage replacement
/// Body: {"stage": "persist", "mode": "fast"|"normal"}
static Handler make_post_hotswap(TradingPlatform& p) {
    return [&p](const Request& req, Response& res) {
        auto req_dto = HotswapRequest::from_json(req.body());
        if (!req_dto) {
            res.status(400).body(qbuem::write(ErrorResponse{"required: stage, mode"})); return;
        }

        if (req_dto->stage != "persist") {
            res.status(400).body(qbuem::write(ErrorResponse{"only 'persist' stage supports hotswap"})); return;
        }

        bool ok;
        if (req_dto->mode == "fast") {
            ok = p.dyn_pipe->hot_swap("persist", make_persist_fast_stage(p.db));
            std::println("[hotswap] persist → FAST mode");
        } else if (req_dto->mode == "normal") {
            ok = p.dyn_pipe->hot_swap("persist", make_persist_stage(p.db));
            std::println("[hotswap] persist → NORMAL mode");
        } else {
            res.status(400).body(qbuem::write(ErrorResponse{"mode must be: fast|normal"})); return;
        }

        res.status(ok ? 200 : 404)
           .header("Content-Type", "application/json")
           .body(qbuem::write(HotswapResponse{req_dto->stage, req_dto->mode, ok}));
    };
}

/// POST /api/v1/admin/toggle — DynamicPipeline stage enable/disable
/// Body: {"stage": "enrich2", "enabled": "true"|"false"}
static Handler make_post_toggle(TradingPlatform& p) {
    return [&p](const Request& req, Response& res) {
        auto req_dto = ToggleRequest::from_json(req.body());
        if (!req_dto) {
            res.status(400).body(qbuem::write(ErrorResponse{"required: stage, enabled"})); return;
        }

        bool ok = p.dyn_pipe->set_enabled(req_dto->stage, req_dto->enabled);
        std::println("[toggle] stage={} enabled={}",
                    req_dto->stage, req_dto->enabled ? "true" : "false");

        res.status(ok ? 200 : 404)
           .header("Content-Type", "application/json")
           .body(qbuem::write(ToggleResponse{req_dto->stage, req_dto->enabled, ok}));
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// §11. Demo Simulation
// ─────────────────────────────────────────────────────────────────────────────

struct DemoOrder {
    std::string account_id;
    std::string symbol;
    double      price;
    int         quantity;
    bool        is_market;
    bool        is_buy;
    const char* desc;
};

static const DemoOrder kDemoOrders[] = {
    // normal limit buy
    {"ACC001", "SAMSUNG",  72000.0, 10, false, true,  "limit buy (normal)"},
    // normal market sell
    {"ACC002", "SKHYNIX",      0.0,  5, true,  false, "market sell (normal)"},
    // insufficient balance (Dave: 1M KRW, notional=23.5M KRW)
    {"ACC004", "KRAFTON", 235000.0, 100, false, true, "insufficient balance → rejected"},
    // unsupported symbol
    {"ACC001", "TESLA",    100.0,    1, false, true,  "unsupported symbol → rejected"},
    // risk limit exceeded (notional=395M KRW)
    {"ACC001", "LGCHEM",  395000.0,1000,false, true,  "risk limit exceeded → rejected"},
    // several normal buy orders
    {"ACC002", "NAVER",   210000.0,  5, false, true,  "limit buy (normal)"},
    {"ACC003", "KAKAO",    55000.0, 20, true,  true,  "market buy (normal)"},
    {"ACC001", "HYUNDAI", 205000.0,  3, false, false, "limit sell (normal)"},
};

static Task<void> run_demo(TradingPlatform& p, Dispatcher& d) {
    std::println("\n╔══════════════════════════════════════════════════════════╗");
    std::println("║       qbuem-stack Trading Platform Demo Start            ║");
    std::println("╚══════════════════════════════════════════════════════════╝\n");

    // brief wait for pipeline workers to fully start
    co_await qbuem::sleep(200);

    // ── Phase 1: process normal orders ──────────────────────────────────
    std::println("─── Phase 1: Order Submission (8 orders) ───────────────────");

    for (size_t i = 0; i < std::size(kDemoOrders); ++i) {
        const auto& demo = kDemoOrders[i];
        std::println("\n[demo {}] {}", i + 1, demo.desc);

        OrderCmd cmd;
        cmd.account_id = demo.account_id;
        cmd.symbol     = demo.symbol;
        cmd.price      = demo.price;
        cmd.quantity   = demo.quantity;
        cmd.is_market  = demo.is_market;
        cmd.is_buy     = demo.is_buy;

        auto ctx = Context{}
            .put(RequestId{"demo-req-" + std::to_string(i + 1)})
            .put(AuthSubject{demo.account_id});

        auto resp_ch = p.begin_order(cmd, ctx);
        if (!resp_ch) {
            std::println("  → [ERROR] pipeline overloaded");
            continue;
        }

        auto result = co_await resp_ch->recv();
        if (!result) {
            std::println("  → [ERROR] no result");
            continue;
        }

        std::println("  → order_id={} status={} exec_price={:.0f} msg={}",
            static_cast<unsigned long long>(result->order_id),
            result->status,
            result->exec_price,
            result->message);
    }

    // ── Phase 2: scale in/out demo ───────────────────────────────────────
    std::println("\n─── Phase 2: Manual Scale In/Out ──────────────────────────");
    std::println("[demo] validate workers (scale_out): 1→2");
    p.validate_act->scale_out(d);
    std::println("[demo] enrich workers (scale_to 3): 2→3");
    p.enrich_act->scale_to(3, d);
    std::println("[demo] risk workers (scale_in): back to min");
    p.risk_act->scale_in();

    // ── Phase 3: DynamicPipeline hot_swap ───────────────────────────────
    std::println("\n─── Phase 3: DynamicPipeline hot_swap (persist → fast) ────");
    p.dyn_pipe->hot_swap("persist", make_persist_fast_stage(p.db));

    // process order after hot_swap
    {
        OrderCmd cmd{.account_id="ACC002", .symbol="POSCO", .price=382000.0,
                     .quantity=1, .is_market=false, .is_buy=true};
        auto ctx = Context{}.put(RequestId{"demo-hotswap-1"}).put(AuthSubject{"ACC002"});
        auto rch = p.begin_order(cmd, ctx);
        if (rch) {
            auto r = co_await rch->recv();
            if (r) std::println("  [hotswap] order_id={} status={} (fast mode)",
                static_cast<unsigned long long>(r->order_id), r->status);
        }
    }

    // ── Phase 4: DynamicPipeline stage toggle (disable enrich2) ──────────
    std::println("\n─── Phase 4: DynamicPipeline stage toggle (enrich2 OFF) ───");
    p.dyn_pipe->set_enabled("enrich2", false);
    std::println("  [toggle] enrich2 disabled (pass-through mode)");

    {
        OrderCmd cmd{.account_id="ACC001", .symbol="SAMSUNG", .price=73000.0,
                     .quantity=5, .is_market=false, .is_buy=true};
        auto ctx = Context{}.put(RequestId{"demo-toggle-1"}).put(AuthSubject{"ACC001"});
        auto rch = p.begin_order(cmd, ctx);
        if (rch) {
            auto r = co_await rch->recv();
            if (r) std::println("  [toggle] order_id={} status={} (enrich2 skipped)",
                static_cast<unsigned long long>(r->order_id), r->status);
        }
    }

    p.dyn_pipe->set_enabled("enrich2", true);  // re-enable
    std::println("  [toggle] enrich2 re-enabled");

    // ── Phase 5: print statistics ────────────────────────────────────────
    std::println("\n─── Phase 5: Final Statistics ──────────────────────────────");
    std::println("  submitted: {}  filled: {}  rejected: {}  DB total: {}",
        static_cast<unsigned long long>(p.submitted.load()),
        static_cast<unsigned long long>(p.filled.load()),
        static_cast<unsigned long long>(p.rejected.load()),
        p.db->count());

    std::println("  queue status: validate={} enrich={} risk={}",
        p.validate_act->input()->size_approx() == 0 ? "empty" : "pending",
        p.enrich_act->input()->size_approx() == 0   ? "empty" : "pending",
        p.risk_act->input()->size_approx() == 0     ? "empty" : "pending");

    std::println("  DynamicPipeline stages: {} ({})",
        p.dyn_pipe->stage_count(),
        [&]() {
            std::string s;
            for (auto& n : p.dyn_pipe->stage_names()) {
                if (!s.empty()) s += " → ";
                s += n;
            }
            return s;
        }());

    std::println("\n╔══════════════════════════════════════════════════════════╗");
    std::println("║       Demo complete. HTTP server running...              ║");
    std::println("║       curl -H 'Authorization: Bearer demo-key' \\        ║");
    std::println("║         -X POST http://localhost:8080/api/v1/orders \\   ║");
    std::println("║         -d '{{\"account_id\":\"ACC001\",\"symbol\":\"SAMSUNG\",\\  ║");
    std::println("║              \"price\":72000,\"quantity\":5,              \\  ║");
    std::println("║              \"type\":\"limit\",\"side\":\"buy\"}}' ║");
    std::println("╚══════════════════════════════════════════════════════════╝\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// §12. main()
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    // ── Pipeline Dispatcher (separate thread pool, isolated from HTTP App) ──
    Dispatcher pipeline_disp(4);
    std::jthread pipeline_thread([&pipeline_disp] { pipeline_disp.run(); });

    // ── Initialize TradingPlatform ───────────────────────────────────────
    TradingPlatform platform;
    platform.setup(pipeline_disp);

    // ── Initialize HTTP App ──────────────────────────────────────────────
    App app(2);  // 2 HTTP reactor threads

    // ── Register middleware chain ────────────────────────────────────────
    app.use(cors(CorsConfig{
        .allow_origin      = "*",
        .allow_methods     = "GET, POST, PUT, DELETE, OPTIONS",
        .allow_headers     = "Content-Type, Authorization, X-Request-ID",
        .allow_credentials = false,
    }));

    app.use(rate_limit(RateLimitConfig{
        .rate_per_sec = 100.0,
        .max_keys     = 10'000,
        .burst        = 20.0,
    }));

    app.use(request_id("X-Request-ID"));

    app.use(hsts(31'536'000, /*include_subdomains=*/true));

    // Bearer auth (applied to all /api/v1/* routes)
    static DemoKeyVerifier verifier;
    app.use(bearer_auth(verifier));

    // ── Register routes ──────────────────────────────────────────────────
    app.post("/api/v1/orders",          make_post_order(platform));
    app.get ("/api/v1/orders",          make_get_orders(platform));
    app.get ("/api/v1/orders/:id",      make_get_order(platform));
    app.del ("/api/v1/orders/:id",      make_cancel_order(platform));
    app.get ("/api/v1/events",          make_sse_events(platform));
    app.get ("/api/v1/stats",           make_get_stats(platform));
    app.post("/api/v1/admin/scale",     make_post_scale(platform));
    app.post("/api/v1/admin/hotswap",   make_post_hotswap(platform));
    app.post("/api/v1/admin/toggle",    make_post_toggle(platform));
    app.health_check("/health");

    // ── Global error handler ─────────────────────────────────────────────
    app.on_error([](std::exception_ptr ep, const Request& req, Response& res) {
        std::string msg;
        try { std::rethrow_exception(ep); }
        catch (const std::exception& e) { msg = e.what(); }
        catch (...)                     { msg = "unknown error"; }
        std::print(stderr, "[error] {} {}: {}\n",
            std::string(req.path()),
            std::string(req.path()), msg);
        res.status(500)
           .header("Content-Type", "application/json")
           .body(qbuem::write(ErrorResponse{msg}));
    });

    // ── Launch demo simulation coroutine ─────────────────────────────────
    pipeline_disp.spawn(run_demo(platform, pipeline_disp));

    // ── Start HTTP server (blocking) ─────────────────────────────────────
    std::println("[server] listening on :8080");
    auto res = app.listen(8080);
    if (!res) {
        std::print(stderr, "[fatal] listen failed: {}\n",
                     res.error().message());
        pipeline_disp.stop();
        pipeline_thread.join();
        return 1;
    }

    // ── Shutdown ──────────────────────────────────────────────────────────
    pipeline_disp.stop();
    pipeline_thread.join();
    return 0;
}
