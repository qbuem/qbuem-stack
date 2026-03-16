/**
 * @file examples/trading_platform.cpp
 * @brief qbuem-stack 종합 플랫폼 예제 — 실시간 주문 처리 거래 서버
 *
 * ## 시스템 구조 (전체 연계)
 *
 *   [HTTP 클라이언트]
 *       │ POST /api/v1/orders  {account_id, symbol, price, quantity, type, side}
 *       ▼
 *   [App 미들웨어 체인]
 *       ├─ CORS          (allow_origin=*)
 *       ├─ RateLimit     (100 req/s, burst=20)
 *       ├─ RequestID     (X-Request-ID 헤더)
 *       ├─ HSTS          (max_age=31536000)
 *       └─ BearerAuth    (DemoKeyVerifier: "demo-key" → user:alice)
 *       │
 *       ▼ JSON 파싱 → OrderCmd + ResponseChannel Context
 *
 *   ┌── [StaticPipeline: Action 체인] ─────────────────────────────────────┐
 *   │   Action<OrderCmd, ValidatedOrder>   validate  {min=1, max=3, auto}  │
 *   │   Action<ValidatedOrder, EnrichedOrder> enrich  {min=2, max=4, auto} │
 *   │   Action<EnrichedOrder, RiskResult>  risk_check {min=1, max=6, slo}  │
 *   └──────────────────────────────────────────────────────────────────────┘
 *       │ ContextualItem<RiskResult> (Context 보존 브릿지 코루틴)
 *       ▼
 *   ┌── [DynamicPipeline<RiskResult>] ─────────────────────────────────────┐
 *   │   Stage "persist"  (MockDB 저장, order_id 생성)   ← hot_swap 가능    │
 *   │   Stage "enrich2"  (추가 계산, set_enabled 가능)                     │
 *   │   Stage "notify"   (ResponseChannel 기록 + MessageBus 발행)          │
 *   └──────────────────────────────────────────────────────────────────────┘
 *       │
 *       ├─ ctx.get<ResponseChannel>() → resp_ch.try_send(OrderResult)
 *       │   ↑
 *       │   └── HTTP 핸들러 co_await resp_ch->recv() → JSON 응답 반환
 *       │
 *       └─ MessageBus.publish("order.completed") → fan-out
 *           ├─ GET /api/v1/events  (SSE 스트림, 실시간 이벤트)
 *           └─ Stats 카운터 업데이트
 *
 *   [자동 스케일러 코루틴]
 *       └─ 500ms 주기로 validate/enrich/risk input queue 깊이 감시
 *           → queue > 50 → scale_out(dispatcher)
 *           → queue <  5 → scale_in()
 *
 * ## API 엔드포인트
 *   POST   /api/v1/orders              주문 제출 (pipeline 처리 후 응답)
 *   GET    /api/v1/orders              전체 주문 목록 (mock DB)
 *   GET    /api/v1/orders/:id          주문 조회
 *   DELETE /api/v1/orders/:id          주문 취소
 *   GET    /api/v1/events              SSE 실시간 이벤트 스트림
 *   GET    /api/v1/stats               파이프라인 상태 / 워커 수 / 큐 깊이
 *   POST   /api/v1/admin/scale         수동 스케일 조정
 *   POST   /api/v1/admin/hotswap       DynamicPipeline 스테이지 교체
 *   POST   /api/v1/admin/toggle        DynamicPipeline 스테이지 활성/비활성
 *   GET    /health                     헬스체크
 */

// ─── Includes ─────────────────────────────────────────────────────────────────

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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace qbuem;
using namespace qbuem::middleware;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// §1. 경량 JSON 헬퍼 (외부 의존성 없음)
// ─────────────────────────────────────────────────────────────────────────────

namespace json {

/// 문자열을 JSON 인코딩합니다 (escape + 따옴표).
inline std::string str(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        if      (c == '"')  { out += "\\\""; }
        else if (c == '\\') { out += "\\\\"; }
        else if (c == '\n') { out += "\\n";  }
        else if (c == '\r') { out += "\\r";  }
        else if (c == '\t') { out += "\\t";  }
        else                { out += c;       }
    }
    out += '"';
    return out;
}

inline std::string num(double v)   { return std::to_string(v); }
inline std::string num(uint64_t v) { return std::to_string(v); }
inline std::string num(int v)      { return std::to_string(v); }
inline std::string boolean(bool v) { return v ? "true" : "false"; }

/// 단순 JSON 오브젝트 빌더.
class Object {
public:
    template <typename V>
    Object& add(std::string_view key, V&& val) {
        if (!first_) buf_ += ',';
        buf_ += str(key); buf_ += ':'; buf_ += std::forward<V>(val);
        first_ = false;
        return *this;
    }
    std::string build() const { return "{" + buf_ + "}"; }
private:
    std::string buf_;
    bool first_ = true;
};

/// 단순 JSON 배열 빌더.
class Array {
public:
    Array& add(std::string val) {
        if (!first_) buf_ += ',';
        buf_ += std::move(val);
        first_ = false;
        return *this;
    }
    std::string build() const { return "[" + buf_ + "]"; }
private:
    std::string buf_;
    bool first_ = true;
};

/// 최소 파서: JSON body에서 문자열 필드를 추출합니다.
inline std::optional<std::string> parse_str(std::string_view body,
                                             std::string_view key) {
    std::string needle = "\"" + std::string(key) + "\"";
    auto pos = body.find(needle);
    if (pos == std::string_view::npos) return std::nullopt;
    pos += needle.size();
    // skip whitespace and ':'
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == ':')) ++pos;
    if (pos >= body.size() || body[pos] != '"') return std::nullopt;
    ++pos; // skip opening quote
    std::string result;
    while (pos < body.size() && body[pos] != '"') {
        if (body[pos] == '\\' && pos + 1 < body.size()) {
            ++pos;
            switch (body[pos]) {
                case '"':  result += '"';  break;
                case '\\': result += '\\'; break;
                case 'n':  result += '\n'; break;
                default:   result += body[pos]; break;
            }
        } else {
            result += body[pos];
        }
        ++pos;
    }
    return result;
}

/// 최소 파서: JSON body에서 숫자 필드를 추출합니다.
inline std::optional<double> parse_double(std::string_view body,
                                           std::string_view key) {
    std::string needle = "\"" + std::string(key) + "\"";
    auto pos = body.find(needle);
    if (pos == std::string_view::npos) return std::nullopt;
    pos += needle.size();
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == ':')) ++pos;
    if (pos >= body.size()) return std::nullopt;
    try {
        size_t consumed = 0;
        double val = std::stod(std::string(body.substr(pos)), &consumed);
        if (consumed == 0) return std::nullopt;
        return val;
    } catch (...) { return std::nullopt; }
}

inline std::optional<int> parse_int(std::string_view body,
                                     std::string_view key) {
    auto v = parse_double(body, key);
    if (!v) return std::nullopt;
    return static_cast<int>(*v);
}

} // namespace json

// ─────────────────────────────────────────────────────────────────────────────
// §2. 도메인 타입 (DTO 설계)
// ─────────────────────────────────────────────────────────────────────────────

/// HTTP로 들어온 주문 명령 (Pipeline 입력 타입).
struct OrderCmd {
    uint64_t    req_id    = 0;       ///< HTTP 요청 고유 번호 (Context에서도 전파)
    std::string account_id;
    std::string symbol;
    double      price     = 0.0;
    int         quantity  = 0;
    bool        is_market = false;   ///< true=시장가, false=지정가
    bool        is_buy    = true;    ///< true=매수, false=매도
};

/// [Static Stage 1] 유효성 검증 결과.
struct ValidatedOrder {
    OrderCmd    cmd;
    bool        valid     = true;
    std::string error_msg;
    double      notional  = 0.0;     ///< price × quantity
};

/// [Static Stage 2] 계좌/시장 데이터 보강 결과.
struct EnrichedOrder {
    ValidatedOrder validated;
    // 유효한 경우에만 채워짐:
    std::string    account_name;
    double         account_balance = 0.0;
    double         market_price    = 0.0;   ///< 현재 시장가
    double         slippage_bps    = 0.0;   ///< 예상 슬리피지 (basis points)
};

/// [Static Stage 3] 리스크 평가 결과 (Static Pipeline 최종 출력).
struct RiskResult {
    EnrichedOrder enriched;
    bool          risk_ok     = false;
    std::string   risk_reason;
    double        var_1d      = 0.0;     ///< 1일 VaR 추정액 (KRW)
};

/// Mock DB 저장 레코드.
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
    std::string reason;                  ///< 거부/취소 사유
};

/// HTTP 응답 DTO (Pipeline → HTTP 핸들러).
struct OrderResult {
    uint64_t    order_id   = 0;
    std::string status;
    double      exec_price = 0.0;
    std::string message;
    bool        success    = false;
};

/// MessageBus 이벤트 (SSE 스트리밍용).
struct OrderEvent {
    uint64_t    order_id   = 0;
    std::string account_id;
    std::string symbol;
    std::string status;
    double      exec_price = 0.0;
    std::string reason;
};

// ─────────────────────────────────────────────────────────────────────────────
// §3. Context 태그 — Pipeline 전 레이어를 통과하는 메타데이터
// ─────────────────────────────────────────────────────────────────────────────

/// HTTP 응답 채널: 마지막 Pipeline Stage가 결과를 여기에 씁니다.
struct ResponseChannel {
    std::shared_ptr<AsyncChannel<OrderResult>> ch;
};

// RequestId, AuthSubject는 context.hpp에 이미 정의된 내장 타입 사용

// ─────────────────────────────────────────────────────────────────────────────
// §4. Mock DB (thread-safe 인메모리 저장소)
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
        for (auto it = orders_.rbegin(); it != orders_.rend() && result.size() < limit; ++it)
            result.push_back(it->second);
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
// §5. Mock 시장 데이터
// ─────────────────────────────────────────────────────────────────────────────

struct MockAccount {
    std::string name;
    double      balance; // KRW
};

static const std::unordered_map<std::string, MockAccount> kAccounts = {
    {"ACC001", {"Alice",   100'000'000.0}},
    {"ACC002", {"Bob",      50'000'000.0}},
    {"ACC003", {"Charlie",  10'000'000.0}},
    {"ACC004", {"Dave",      1'000'000.0}},  // 잔고 부족 테스트용
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
// §6. 인증 토큰 검증기 (ITokenVerifier 구현)
// ─────────────────────────────────────────────────────────────────────────────

class DemoKeyVerifier : public ITokenVerifier {
public:
    std::optional<TokenClaims> verify(std::string_view token) noexcept override {
        // 데모용: "demo-key" 허용, "demo-key-dave" → Dave 계정
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
// §7. JSON 변환 헬퍼
// ─────────────────────────────────────────────────────────────────────────────

static std::string order_record_to_json(const OrderRecord& r) {
    return json::Object{}
        .add("order_id",    json::num(r.order_id))
        .add("account_id",  json::str(r.account_id))
        .add("symbol",      json::str(r.symbol))
        .add("side",        json::str(r.side))
        .add("type",        json::str(r.type))
        .add("price",       json::num(r.price))
        .add("quantity",    json::num(r.quantity))
        .add("status",      json::str(r.status))
        .add("exec_price",  json::num(r.exec_price))
        .add("created_at",  json::str(r.created_at))
        .add("reason",      json::str(r.reason))
        .build();
}

static std::string order_result_to_json(const OrderResult& r) {
    return json::Object{}
        .add("order_id",   json::num(r.order_id))
        .add("status",     json::str(r.status))
        .add("exec_price", json::num(r.exec_price))
        .add("message",    json::str(r.message))
        .add("success",    json::boolean(r.success))
        .build();
}

static std::string json_error(std::string_view msg) {
    return json::Object{}.add("error", json::str(msg)).build();
}

static std::string order_event_to_json(const OrderEvent& e) {
    return json::Object{}
        .add("order_id",   json::num(e.order_id))
        .add("account_id", json::str(e.account_id))
        .add("symbol",     json::str(e.symbol))
        .add("status",     json::str(e.status))
        .add("exec_price", json::num(e.exec_price))
        .add("reason",     json::str(e.reason))
        .build();
}

// ─────────────────────────────────────────────────────────────────────────────
// §8. Static Pipeline 스테이지 함수
// ─────────────────────────────────────────────────────────────────────────────

/// Stage 1: 유효성 검증 (포맷/범위 체크)
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

    std::printf("  [validate #%02llu] worker=%zu %s %s %s qty=%d notional=%.0f\n",
        static_cast<unsigned long long>(cmd.req_id),
        env.worker_idx,
        cmd.is_buy ? "BUY" : "SELL",
        cmd.symbol.c_str(),
        cmd.is_market ? "MARKET" : "LIMIT",
        cmd.quantity,
        v.notional);

    co_return v;
}

/// Stage 2: 계좌/시장 데이터 보강 — 외부 데이터 조회 시뮬레이션
static Task<Result<EnrichedOrder>> stage_enrich(ValidatedOrder v, ActionEnv env) {
    EnrichedOrder e;
    e.validated = v;

    if (!v.valid) co_return e;  // 이미 실패 → 패스스루

    auto acct_it = kAccounts.find(v.cmd.account_id);
    if (acct_it == kAccounts.end()) {
        e.validated.valid     = false;
        e.validated.error_msg = "account not found: " + v.cmd.account_id;
        co_return e;
    }

    e.account_name    = acct_it->second.name;
    e.account_balance = acct_it->second.balance;
    e.market_price    = kMarketPrices.at(v.cmd.symbol);

    // 슬리피지 계산 (지정가 주문: 시장가 대비 편차)
    if (!v.cmd.is_market) {
        e.slippage_bps = std::abs(v.cmd.price - e.market_price) / e.market_price * 10000.0;
    }

    // 잔고 부족 체크 (매수 시)
    if (v.cmd.is_buy && v.notional > e.account_balance) {
        e.validated.valid     = false;
        e.validated.error_msg = "insufficient balance";
    }

    std::printf("  [enrich   #%02llu] worker=%zu acct=%s balance=%.0f mkt_price=%.0f slippage=%.1fbps\n",
        static_cast<unsigned long long>(v.cmd.req_id),
        env.worker_idx,
        e.account_name.c_str(),
        e.account_balance,
        e.market_price,
        e.slippage_bps);

    co_return e;
}

/// Stage 3: 리스크 평가 — VaR 추정, 한도 체크
static Task<Result<RiskResult>> stage_risk_check(EnrichedOrder e, ActionEnv env) {
    RiskResult r;
    r.enriched = e;

    if (!e.validated.valid) {
        r.risk_ok     = false;
        r.risk_reason = e.validated.error_msg;
        co_return r;
    }

    // 1일 VaR 추정: 간이 공식 (2% 일변동성 가정)
    r.var_1d = e.validated.notional * 0.02;

    // 리스크 한도 체크
    constexpr double kMaxNotional = 50'000'000.0;   // 5천만원
    constexpr double kMaxVaR      =    500'000.0;   //   50만원

    if (e.validated.notional > kMaxNotional) {
        r.risk_ok     = false;
        r.risk_reason = "notional exceeds limit (max 50M KRW)";
    } else if (r.var_1d > kMaxVaR) {
        r.risk_ok     = false;
        r.risk_reason = "VaR exceeds limit";
    } else {
        r.risk_ok = true;
    }

    std::printf("  [risk     #%02llu] worker=%zu var1d=%.0f ok=%s %s\n",
        static_cast<unsigned long long>(e.validated.cmd.req_id),
        env.worker_idx,
        r.var_1d,
        r.risk_ok ? "YES" : "NO",
        r.risk_ok ? "" : r.risk_reason.c_str());

    co_return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// §9. Dynamic Pipeline 스테이지 함수
// ─────────────────────────────────────────────────────────────────────────────

/// Dynamic Stage "persist": MockDB 저장, order_id 생성
/// — hot_swap 대상 (persist_fast 버전으로 교체 가능)
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
            rec.exec_price = risk.enriched.market_price;  // 시장가로 체결
        }

        uint64_t oid = db->save(rec);

        // order_id를 RiskResult에 주입 (notify stage에서 사용)
        // Context에 넣는 대신 단순히 cmd.req_id → order_id 매핑 저장
        // (여기서는 간단히 risk_reason에 encode)
        risk.enriched.validated.cmd.req_id = oid;  // 재활용: order_id로 덮어씀

        std::printf("  [persist  #%02llu] worker=%zu status=%s exec_price=%.0f\n",
            static_cast<unsigned long long>(oid),
            env.worker_idx,
            rec.status.c_str(),
            rec.exec_price);

        co_return risk;
    };
}

/// Dynamic Stage "persist_fast": hot_swap 교체 대상 (간소화 버전)
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

        std::printf("  [persist★ #%02llu] worker=%zu [FAST MODE] status=%s\n",
            static_cast<unsigned long long>(oid), env.worker_idx, rec.status.c_str());

        co_return risk;
    };
}

/// Dynamic Stage "enrich2": 추가 계산 (set_enabled 시연 — 비활성 시 패스스루)
static Task<Result<RiskResult>> stage_enrich2(RiskResult risk, ActionEnv env) {
    // 슬리피지 비용 추가 계산 (선택적 스테이지)
    if (risk.enriched.validated.valid) {
        double slippage_cost = risk.enriched.validated.notional
                               * risk.enriched.slippage_bps / 10000.0;
        std::printf("  [enrich2  #%02llu] worker=%zu slippage_cost=%.2f KRW\n",
            static_cast<unsigned long long>(risk.enriched.validated.cmd.req_id),
            env.worker_idx, slippage_cost);
    }
    co_return risk;
}

/// Dynamic Stage "notify": ResponseChannel에 결과 쓰기 + MessageBus 발행
static auto make_notify_stage(std::shared_ptr<MessageBus> bus,
                               std::atomic<uint64_t>& filled,
                               std::atomic<uint64_t>& rejected) {
    return [bus, &filled, &rejected](RiskResult risk, ActionEnv env) -> Task<Result<RiskResult>> {
        uint64_t oid = risk.enriched.validated.cmd.req_id;  // persist stage에서 설정
        bool success = risk.enriched.validated.valid && risk.risk_ok;

        // ── 1) ResponseChannel에 HTTP 응답 데이터 전송 ─────────────────────
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
            rch->ch->try_send(result);  // HTTP 핸들러의 co_await recv()를 깨움
        }

        // ── 2) 통계 업데이트 ───────────────────────────────────────────────
        if (success)
            filled.fetch_add(1, std::memory_order_relaxed);
        else
            rejected.fetch_add(1, std::memory_order_relaxed);

        // ── 3) MessageBus 발행 (SSE 팬아웃) ───────────────────────────────
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

        std::printf("  [notify   #%02llu] worker=%zu → HTTP응답 전송, event 발행\n",
            static_cast<unsigned long long>(oid), env.worker_idx);

        co_return risk;
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// §10. TradingPlatform — 전체 상태 보유 구조체
// ─────────────────────────────────────────────────────────────────────────────

struct TradingPlatform {
    // ── 인프라 ────────────────────────────────────────────────────────────
    std::shared_ptr<MockDB>     db  = std::make_shared<MockDB>();
    std::shared_ptr<MessageBus> bus = std::make_shared<MessageBus>();

    // ── Static Pipeline Actions (스케일 제어를 위해 직접 보유) ───────────
    // 타입 별칭
    using VAction = Action<OrderCmd, ValidatedOrder>;
    using EAction = Action<ValidatedOrder, EnrichedOrder>;
    using RAction = Action<EnrichedOrder, RiskResult>;

    std::shared_ptr<VAction> validate_act;
    std::shared_ptr<EAction> enrich_act;
    std::shared_ptr<RAction> risk_act;

    // Static Pipeline의 최종 출력 채널 (브릿지 코루틴에서 읽음)
    std::shared_ptr<AsyncChannel<ContextualItem<RiskResult>>> risk_out;

    // ── Dynamic Pipeline ──────────────────────────────────────────────────
    std::shared_ptr<DynamicPipeline<RiskResult>> dyn_pipe;

    // ── Stats ─────────────────────────────────────────────────────────────
    std::atomic<uint64_t> submitted{0};
    std::atomic<uint64_t> filled{0};
    std::atomic<uint64_t> rejected{0};
    std::atomic<bool>     auto_scale_on{true};
    std::atomic<uint64_t> req_counter{0};

    // ── Dispatcher ref (scale_out 호출에 필요) ────────────────────────────
    Dispatcher* disp = nullptr;

    /// 모든 파이프라인과 버스를 시작합니다.
    void setup(Dispatcher& d) {
        disp = &d;
        bus->start(d);

        // ── Static Pipeline Actions 생성 ─────────────────────────────────

        validate_act = std::make_shared<VAction>(
            stage_validate,
            VAction::Config{
                .min_workers = 1,
                .max_workers = 3,
                .channel_cap = 512,
                .auto_scale  = true,
                .slo = SloConfig{
                    .p99_target_us  = 5'000,   // 5ms p99
                    .error_budget   = 0.001,
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
                    .p99_target_us  = 50'000,  // 50ms p99
                    .error_budget   = 0.005,
                },
            });

        // ── Static Actions 수동 연결 및 시작 ────────────────────────────
        // 시작 순서: 마지막 Action부터 (출력 채널 필요)
        risk_out = std::make_shared<AsyncChannel<ContextualItem<RiskResult>>>(512);
        risk_act->start(d, risk_out);                        // risk → risk_out
        enrich_act->start(d, risk_act->input());             // enrich → risk input
        validate_act->start(d, enrich_act->input());         // validate → enrich input

        // ── Dynamic Pipeline 생성 ─────────────────────────────────────────
        dyn_pipe = std::make_shared<DynamicPipeline<RiskResult>>(
            DynamicPipeline<RiskResult>::Config{
                .default_channel_cap = 256,
                .default_workers     = 2,
            });

        dyn_pipe->add_stage("persist",  make_persist_stage(db));
        dyn_pipe->add_stage("enrich2",  stage_enrich2);
        dyn_pipe->add_stage("notify",   make_notify_stage(bus, filled, rejected));

        dyn_pipe->start(d);

        // ── Static → Dynamic 브릿지 코루틴 (Context 보존) ─────────────
        d.spawn([this]() mutable -> Task<void> {
            for (;;) {
                auto item = co_await risk_out->recv();
                if (!item) break;
                // Context 보존: ResponseChannel, RequestId, AuthSubject 모두 전달
                co_await dyn_pipe->push(item->value, item->ctx);
            }
        }());

        // ── 자동 스케일러 코루틴 ─────────────────────────────────────────
        d.spawn([this]() mutable -> Task<void> {
            for (;;) {
                // 500ms 간격 모니터링
                struct Sleep500ms {
                    bool await_ready() noexcept { return false; }
                    void await_suspend(std::coroutine_handle<> h) noexcept {
                        std::thread([h]() mutable {
                            std::this_thread::sleep_for(500ms);
                            if (auto* r = Reactor::current()) r->post([h]{ h.resume(); });
                            else h.resume();
                        }).detach();
                    }
                    void await_resume() noexcept {}
                };
                co_await Sleep500ms{};

                if (!auto_scale_on.load()) continue;

                // validate 큐 깊이 모니터링 → 스케일 결정
                size_t vdepth = validate_act->input()->size();
                size_t edepth = enrich_act->input()->size();
                size_t rdepth = risk_act->input()->size();

                if (vdepth > 50) {
                    validate_act->scale_out(*disp);
                    std::printf("[autoscale] validate queue=%zu → scale_out\n", vdepth);
                } else if (vdepth < 5) {
                    validate_act->scale_in();
                }

                if (edepth > 50) {
                    enrich_act->scale_out(*disp);
                    std::printf("[autoscale] enrich queue=%zu → scale_out\n", edepth);
                } else if (edepth < 5) {
                    enrich_act->scale_in();
                }

                if (rdepth > 100) {
                    risk_act->scale_out(*disp);
                    std::printf("[autoscale] risk queue=%zu → scale_out\n", rdepth);
                } else if (rdepth < 5) {
                    risk_act->scale_in();
                }
            }
        }());

        std::puts("[platform] setup complete: static(3 actions) + dynamic(3 stages) + autoscaler");
    }

    /// 주문을 파이프라인에 제출합니다.
    /// @returns 결과를 받을 채널 (co_await recv()로 대기)
    std::shared_ptr<AsyncChannel<OrderResult>>
    begin_order(OrderCmd cmd, Context base_ctx) {
        auto resp_ch = std::make_shared<AsyncChannel<OrderResult>>(1);
        auto ctx = base_ctx
            .put(ResponseChannel{resp_ch});
        cmd.req_id = ++req_counter;
        // non-blocking try: 포화 시 nullptr 반환 (호출자가 503 처리)
        if (!validate_act->try_push(cmd, ctx))
            return nullptr;
        submitted.fetch_add(1, std::memory_order_relaxed);
        return resp_ch;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// §11. HTTP 핸들러
// ─────────────────────────────────────────────────────────────────────────────

/// POST /api/v1/orders — 주문 제출 (Pipeline 처리 후 응답)
static AsyncHandler make_post_order(TradingPlatform& p) {
    return [&p](const Request& req, Response& res) -> Task<void> {
        auto body = req.body();

        // JSON 파싱
        auto account_id = json::parse_str(body, "account_id");
        auto symbol     = json::parse_str(body, "symbol");
        auto price      = json::parse_double(body, "price");
        auto quantity   = json::parse_int(body, "quantity");
        auto type_str   = json::parse_str(body, "type");
        auto side_str   = json::parse_str(body, "side");

        if (!account_id || !symbol || !quantity || !type_str || !side_str) {
            res.status(400)
               .header("Content-Type", "application/json")
               .body(json_error("required: account_id, symbol, quantity, type, side"));
            co_return;
        }

        // 인증 정보를 Context에 주입
        auto ctx = Context{}
            .put(RequestId{std::string(req.header("X-Request-ID"))})
            .put(AuthSubject{std::string(req.header("X-Auth-Sub"))});

        OrderCmd cmd;
        cmd.account_id = *account_id;
        cmd.symbol     = *symbol;
        cmd.price      = price.value_or(0.0);
        cmd.quantity   = *quantity;
        cmd.is_market  = (*type_str == "market");
        cmd.is_buy     = (*side_str == "buy");

        // Pipeline에 제출 (비동기)
        auto resp_ch = p.begin_order(cmd, ctx);
        if (!resp_ch) {
            res.status(503)
               .header("Content-Type", "application/json")
               .body(json_error("pipeline overloaded, retry later"));
            co_return;
        }

        // Pipeline 처리 완료 대기 (cross-reactor safe)
        auto result = co_await resp_ch->recv();
        if (!result) {
            res.status(500)
               .header("Content-Type", "application/json")
               .body(json_error("internal pipeline error"));
            co_return;
        }

        res.status(result->success ? 201 : 422)
           .header("Content-Type", "application/json")
           .body(order_result_to_json(*result));
    };
}

/// GET /api/v1/orders — 전체 주문 목록
static Handler make_get_orders(TradingPlatform& p) {
    return [&p](const Request& req, Response& res) {
        auto orders = p.db->list(100);
        json::Array arr;
        for (auto& r : orders) arr.add(order_record_to_json(r));
        res.status(200)
           .header("Content-Type", "application/json")
           .body(arr.build());
    };
}

/// GET /api/v1/orders/:id — 단일 주문 조회
static Handler make_get_order(TradingPlatform& p) {
    return [&p](const Request& req, Response& res) {
        auto id_str = req.param("id");
        uint64_t id = 0;
        try { id = std::stoull(std::string(id_str)); }
        catch (...) {
            res.status(400).body(json_error("invalid order id"));
            return;
        }
        auto rec = p.db->get(id);
        if (!rec) {
            res.status(404).body(json_error("order not found"));
            return;
        }
        res.status(200)
           .header("Content-Type", "application/json")
           .body(order_record_to_json(*rec));
    };
}

/// DELETE /api/v1/orders/:id — 주문 취소
static Handler make_cancel_order(TradingPlatform& p) {
    return [&p](const Request& req, Response& res) {
        auto id_str = req.param("id");
        uint64_t id = 0;
        try { id = std::stoull(std::string(id_str)); } catch (...) {
            res.status(400).body(json_error("invalid order id")); return;
        }
        auto rec = p.db->get(id);
        if (!rec) { res.status(404).body(json_error("order not found")); return; }
        if (rec->status != "filled") {
            res.status(409).body(json_error("only filled orders can be cancelled")); return;
        }
        p.db->update_status(id, "cancelled", "user requested");
        res.status(200)
           .header("Content-Type", "application/json")
           .body(json::Object{}.add("order_id", json::num(id))
                               .add("status", json::str("cancelled")).build());
    };
}

/// GET /api/v1/events — SSE 실시간 이벤트 스트림
static AsyncHandler make_sse_events(TradingPlatform& p) {
    return [&p](const Request& /*req*/, Response& res) -> Task<void> {
        // MessageBus "order.completed" 구독 (스트리밍)
        auto stream = p.bus->subscribe_stream<OrderEvent>("order.completed", 64);

        SseStream sse(res);
        sse.send(json::Object{}.add("message", json::str("connected"))
                               .add("topic",   json::str("order.completed"))
                               .build(),
                 "connected");

        // 최대 30개 이벤트 수신 후 종료 (데모용)
        for (int i = 0; i < 30; ++i) {
            auto ev = co_await stream->recv();
            if (!ev) break;
            sse.send(order_event_to_json(*ev), "order");
        }
        sse.close();
    };
}

/// GET /api/v1/stats — 파이프라인 상태 및 워커 수
static Handler make_get_stats(TradingPlatform& p) {
    return [&p](const Request& /*req*/, Response& res) {
        size_t dyn_stages = p.dyn_pipe->stage_count();
        auto stage_names = p.dyn_pipe->stage_names();

        json::Array stage_arr;
        for (auto& n : stage_names) stage_arr.add(json::str(n));

        std::string stats = json::Object{}
            // 주문 통계
            .add("submitted",      json::num(static_cast<uint64_t>(p.submitted.load())))
            .add("filled",         json::num(static_cast<uint64_t>(p.filled.load())))
            .add("rejected",       json::num(static_cast<uint64_t>(p.rejected.load())))
            .add("db_total",       json::num(static_cast<uint64_t>(p.db->count())))
            // Static Pipeline 큐 깊이
            .add("validate_queue", json::num(static_cast<uint64_t>(p.validate_act->input()->size())))
            .add("enrich_queue",   json::num(static_cast<uint64_t>(p.enrich_act->input()->size())))
            .add("risk_queue",     json::num(static_cast<uint64_t>(p.risk_act->input()->size())))
            // Dynamic Pipeline
            .add("dyn_stages",     json::num(static_cast<int>(dyn_stages)))
            .add("dyn_stage_names", stage_arr.build())
            // 설정
            .add("auto_scale",     json::boolean(p.auto_scale_on.load()))
            .build();

        res.status(200)
           .header("Content-Type", "application/json")
           .body(stats);
    };
}

/// POST /api/v1/admin/scale — 수동 스케일 조정
/// Body: {"stage": "validate"|"enrich"|"risk", "workers": N}
static Handler make_post_scale(TradingPlatform& p) {
    return [&p](const Request& req, Response& res) {
        auto body    = req.body();
        auto stage   = json::parse_str(body, "stage");
        auto workers = json::parse_int(body, "workers");

        if (!stage || !workers) {
            res.status(400).body(json_error("required: stage, workers")); return;
        }

        if (*stage == "validate") {
            p.validate_act->scale_to(static_cast<size_t>(*workers), *p.disp);
        } else if (*stage == "enrich") {
            p.enrich_act->scale_to(static_cast<size_t>(*workers), *p.disp);
        } else if (*stage == "risk") {
            p.risk_act->scale_to(static_cast<size_t>(*workers), *p.disp);
        } else {
            res.status(400).body(json_error("stage must be: validate|enrich|risk")); return;
        }

        res.status(200)
           .header("Content-Type", "application/json")
           .body(json::Object{}
               .add("stage",   json::str(*stage))
               .add("workers", json::num(*workers))
               .add("message", json::str("scale applied"))
               .build());
    };
}

/// POST /api/v1/admin/hotswap — DynamicPipeline "persist" 스테이지 교체
/// Body: {"stage": "persist", "mode": "fast"|"normal"}
static Handler make_post_hotswap(TradingPlatform& p) {
    return [&p](const Request& req, Response& res) {
        auto body  = req.body();
        auto stage = json::parse_str(body, "stage");
        auto mode  = json::parse_str(body, "mode");

        if (!stage || !mode) {
            res.status(400).body(json_error("required: stage, mode")); return;
        }

        if (*stage != "persist") {
            res.status(400).body(json_error("only 'persist' stage supports hotswap")); return;
        }

        bool ok;
        if (*mode == "fast") {
            ok = p.dyn_pipe->hot_swap("persist", make_persist_fast_stage(p.db));
            std::puts("[hotswap] persist → FAST mode");
        } else if (*mode == "normal") {
            ok = p.dyn_pipe->hot_swap("persist", make_persist_stage(p.db));
            std::puts("[hotswap] persist → NORMAL mode");
        } else {
            res.status(400).body(json_error("mode must be: fast|normal")); return;
        }

        res.status(ok ? 200 : 404)
           .header("Content-Type", "application/json")
           .body(json::Object{}
               .add("stage",  json::str(*stage))
               .add("mode",   json::str(*mode))
               .add("success", json::boolean(ok))
               .build());
    };
}

/// POST /api/v1/admin/toggle — DynamicPipeline 스테이지 활성/비활성
/// Body: {"stage": "enrich2", "enabled": true|false}
static Handler make_post_toggle(TradingPlatform& p) {
    return [&p](const Request& req, Response& res) {
        auto body    = req.body();
        auto stage   = json::parse_str(body, "stage");
        auto enabled = json::parse_str(body, "enabled");  // "true"|"false"

        if (!stage || !enabled) {
            res.status(400).body(json_error("required: stage, enabled")); return;
        }

        bool en = (*enabled == "true");
        bool ok = p.dyn_pipe->set_enabled(*stage, en);

        std::printf("[toggle] stage=%s enabled=%s\n", stage->c_str(), en ? "true" : "false");

        res.status(ok ? 200 : 404)
           .header("Content-Type", "application/json")
           .body(json::Object{}
               .add("stage",   json::str(*stage))
               .add("enabled", json::boolean(en))
               .add("success", json::boolean(ok))
               .build());
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// §12. 데모 시뮬레이션
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
    // 정상 지정가 매수
    {"ACC001", "SAMSUNG",  72000.0, 10, false, true,  "지정가 매수 (정상)"},
    // 정상 시장가 매도
    {"ACC002", "SKHYNIX",      0.0,  5, true,  false, "시장가 매도 (정상)"},
    // 잔고 부족 (Dave: 100만원, notional=2350만원)
    {"ACC004", "KRAFTON", 235000.0, 100, false, true, "잔고 부족 → 거부"},
    // 미지원 심볼
    {"ACC001", "TESLA",    100.0,    1, false, true,  "미지원 심볼 → 거부"},
    // 리스크 한도 초과 (notional=39,500만원)
    {"ACC001", "LGCHEM",  395000.0,1000,false, true,  "리스크 한도 초과 → 거부"},
    // 정상 매수 여러 건
    {"ACC002", "NAVER",   210000.0,  5, false, true,  "지정가 매수 (정상)"},
    {"ACC003", "KAKAO",    55000.0, 20, true,  true,  "시장가 매수 (정상)"},
    {"ACC001", "HYUNDAI", 205000.0,  3, false, false, "지정가 매도 (정상)"},
};

static Task<void> run_demo(TradingPlatform& p, Dispatcher& d) {
    std::puts("\n╔══════════════════════════════════════════════════════════╗");
    std::puts("║       qbuem-stack 거래 플랫폼 데모 시작                  ║");
    std::puts("╚══════════════════════════════════════════════════════════╝\n");

    // 잠시 대기 (파이프라인 워커 완전 시작 대기)
    struct Wait {
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) noexcept {
            std::thread([h]{ std::this_thread::sleep_for(200ms); h.resume(); }).detach();
        }
        void await_resume() noexcept {}
    };
    co_await Wait{};

    // ── Phase 1: 정상 주문 처리 ─────────────────────────────────────────
    std::puts("─── Phase 1: 주문 제출 (8건) ───────────────────────────────");

    for (size_t i = 0; i < std::size(kDemoOrders); ++i) {
        const auto& demo = kDemoOrders[i];
        std::printf("\n[demo %zu] %s\n", i + 1, demo.desc);

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
            std::puts("  → [ERROR] pipeline overloaded");
            continue;
        }

        auto result = co_await resp_ch->recv();
        if (!result) {
            std::puts("  → [ERROR] no result");
            continue;
        }

        std::printf("  → order_id=%llu status=%s exec_price=%.0f msg=%s\n",
            static_cast<unsigned long long>(result->order_id),
            result->status.c_str(),
            result->exec_price,
            result->message.c_str());
    }

    // ── Phase 2: 스케일 in/out 시연 ─────────────────────────────────────
    std::puts("\n─── Phase 2: Manual Scale In/Out ──────────────────────────");
    std::printf("[demo] validate workers (scale_out): 1→2\n");
    p.validate_act->scale_out(d);
    std::printf("[demo] enrich workers (scale_to 3): 2→3\n");
    p.enrich_act->scale_to(3, d);
    std::printf("[demo] risk workers (scale_in): back to min\n");
    p.risk_act->scale_in();

    // ── Phase 3: DynamicPipeline hot_swap ───────────────────────────────
    std::puts("\n─── Phase 3: DynamicPipeline hot_swap (persist → fast) ────");
    p.dyn_pipe->hot_swap("persist", make_persist_fast_stage(p.db));

    // hot_swap 후 주문 처리
    {
        OrderCmd cmd{"ACC002", "POSCO", 382000.0, 1, false, true};
        cmd.is_buy = true;
        auto ctx = Context{}.put(RequestId{"demo-hotswap-1"}).put(AuthSubject{"ACC002"});
        auto rch = p.begin_order(cmd, ctx);
        if (rch) {
            auto r = co_await rch->recv();
            if (r) std::printf("  [hotswap] order_id=%llu status=%s (fast mode)\n",
                static_cast<unsigned long long>(r->order_id), r->status.c_str());
        }
    }

    // ── Phase 4: DynamicPipeline stage toggle (enrich2 비활성화) ─────────
    std::puts("\n─── Phase 4: DynamicPipeline stage toggle (enrich2 OFF) ───");
    p.dyn_pipe->set_enabled("enrich2", false);
    std::puts("  [toggle] enrich2 disabled (pass-through mode)");

    {
        OrderCmd cmd{"ACC001", "SAMSUNG", 73000.0, 5, false, true};
        auto ctx = Context{}.put(RequestId{"demo-toggle-1"}).put(AuthSubject{"ACC001"});
        auto rch = p.begin_order(cmd, ctx);
        if (rch) {
            auto r = co_await rch->recv();
            if (r) std::printf("  [toggle] order_id=%llu status=%s (enrich2 skipped)\n",
                static_cast<unsigned long long>(r->order_id), r->status.c_str());
        }
    }

    p.dyn_pipe->set_enabled("enrich2", true);  // 재활성화
    std::puts("  [toggle] enrich2 re-enabled");

    // ── Phase 5: 통계 출력 ───────────────────────────────────────────────
    std::puts("\n─── Phase 5: 최종 통계 ─────────────────────────────────────");
    std::printf("  제출: %llu  체결: %llu  거부: %llu  DB총: %zu\n",
        static_cast<unsigned long long>(p.submitted.load()),
        static_cast<unsigned long long>(p.filled.load()),
        static_cast<unsigned long long>(p.rejected.load()),
        p.db->count());

    std::printf("  큐깊이: validate=%zu enrich=%zu risk=%zu\n",
        p.validate_act->input()->size(),
        p.enrich_act->input()->size(),
        p.risk_act->input()->size());

    std::printf("  DynamicPipeline stages: %zu (%s)\n",
        p.dyn_pipe->stage_count(),
        [&]() {
            std::string s;
            for (auto& n : p.dyn_pipe->stage_names()) {
                if (!s.empty()) s += " → ";
                s += n;
            }
            return s;
        }().c_str());

    std::puts("\n╔══════════════════════════════════════════════════════════╗");
    std::puts("║       데모 완료. HTTP 서버 실행 중...                    ║");
    std::puts("║       curl -H 'Authorization: Bearer demo-key' \\        ║");
    std::puts("║         -X POST http://localhost:8080/api/v1/orders \\   ║");
    std::puts("║         -d '{\"account_id\":\"ACC001\",\"symbol\":\"SAMSUNG\",\\  ║");
    std::puts("║              \"price\":72000,\"quantity\":5,              \\  ║");
    std::puts("║              \"type\":\"limit\",\"side\":\"buy\"}'            ║");
    std::puts("╚══════════════════════════════════════════════════════════╝\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// §13. main()
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    // ── Pipeline Dispatcher (HTTP App과 분리된 별도 스레드 풀) ───────────
    Dispatcher pipeline_disp(4);
    std::thread pipeline_thread([&pipeline_disp] { pipeline_disp.run(); });

    // ── Trading Platform 초기화 ──────────────────────────────────────────
    TradingPlatform platform;
    platform.setup(pipeline_disp);

    // ── HTTP App 초기화 ─────────────────────────────────────────────────
    App app(2);  // HTTP 리액터 스레드 2개

    // ── 미들웨어 체인 등록 ───────────────────────────────────────────────
    app.use(cors(CorsConfig{
        .allow_origin      = "*",
        .allow_methods     = "GET, POST, PUT, DELETE, OPTIONS",
        .allow_headers     = "Content-Type, Authorization, X-Request-ID",
        .allow_credentials = false,
    }));

    app.use(rate_limit(RateLimitConfig{
        .rate_per_sec = 100.0,
        .burst        = 20.0,
        .max_keys     = 10'000,
    }));

    app.use(request_id("X-Request-ID"));

    app.use(hsts(HstsConfig{
        .max_age            = 31'536'000,
        .include_subdomains = true,
    }));

    // Bearer 인증 (모든 /api/v1/* 에 적용)
    static DemoKeyVerifier verifier;
    app.use(bearer_auth(verifier));

    // ── 라우트 등록 ─────────────────────────────────────────────────────
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

    // ── 전역 에러 핸들러 ─────────────────────────────────────────────────
    app.on_error([](std::exception_ptr ep, const Request& req, Response& res) {
        std::string msg;
        try { std::rethrow_exception(ep); }
        catch (const std::exception& e) { msg = e.what(); }
        catch (...)                     { msg = "unknown error"; }
        std::fprintf(stderr, "[error] %s %s: %s\n",
            std::string(req.path()).c_str(),
            std::string(req.path()).c_str(), msg.c_str());
        res.status(500)
           .header("Content-Type", "application/json")
           .body(json_error(msg));
    });

    // ── 데모 시뮬레이션 코루틴 시작 ─────────────────────────────────────
    pipeline_disp.spawn(run_demo(platform, pipeline_disp));

    // ── HTTP 서버 시작 (blocking) ────────────────────────────────────────
    std::puts("[server] listening on :8080");
    auto res = app.listen(8080);
    if (!res) {
        std::fprintf(stderr, "[fatal] listen failed: %s\n",
                     res.error().message().c_str());
        pipeline_disp.stop();
        pipeline_thread.join();
        return 1;
    }

    // ── 종료 처리 ────────────────────────────────────────────────────────
    pipeline_disp.stop();
    pipeline_thread.join();
    return 0;
}
