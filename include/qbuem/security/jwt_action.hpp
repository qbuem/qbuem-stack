#pragma once

/**
 * @file qbuem/security/jwt_action.hpp
 * @brief JWT 검증 Pipeline Action — SIMD 파서 + ITokenVerifier 통합.
 * @defgroup qbuem_security_jwt_action JwtAuthAction
 * @ingroup qbuem_security
 *
 * ## 개요
 * `SIMDJwtParser`와 `ITokenVerifier`를 결합하여
 * HTTP 요청의 Bearer 토큰을 **Pipeline Stage**에서 검증합니다.
 *
 * ## Pipeline 통합
 * @code
 * auto pipeline = PipelineBuilder<HttpRequest>()
 *     .add<JwtAuthAction>(verifier, JwtAuthConfig{.leeway_sec = 5})
 *     .add<ApiHandler>()
 *     .build();
 * @endcode
 *
 * ## 검증 흐름
 * ```
 * Authorization: Bearer <token>
 *          │
 *          ▼
 * SIMDJwtParser::parse()        ← dot-scan, Base64url 검증
 *          │
 *          ▼
 * JwtView::is_expired()         ← exp 클레임 확인
 *          │
 *          ▼
 * ITokenVerifier::verify()      ← HMAC/RSA 서명 검증 (사용자 구현)
 *          │
 *          ▼
 * Context::put<JwtClaims>()     ← 검증된 클레임을 Context에 주입
 * ```
 *
 * ## 캐싱 (선택)
 * `JwtAuthConfig::cache_size > 0`이면 검증 결과를 LRU 캐시에 저장합니다.
 * 동일 토큰 재검증 시 파싱/서명 검증을 생략합니다.
 * 캐시 키는 토큰 서명 파트의 상위 64-bit(타이밍 불변 비교)입니다.
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/middleware/token_auth.hpp>
#include <qbuem/pipeline/action.hpp>
#include <qbuem/pipeline/context.hpp>
#include <qbuem/security/simd_jwt.hpp>

#include <atomic>
#include <cstring>
#include <optional>
#include <string_view>

namespace qbuem::security {

// ─── ActionResult ─────────────────────────────────────────────────────────────

/** @brief Pipeline 미들웨어 액션 결과 — 처리 계속/중단 결정. */
struct ActionResult {
    bool            should_continue{true};
    std::error_code error;

    /// @brief 다음 단계로 계속 진행합니다.
    static ActionResult next() noexcept { return {true, {}}; }

    /// @brief 파이프라인을 중단하고 에러를 반환합니다.
    static ActionResult stop(std::error_code ec) noexcept { return {false, ec}; }
};

// ─── JwtClaims (Context 주입용) ──────────────────────────────────────────────

/**
 * @brief 검증된 JWT 클레임 — Context에 주입되는 타입.
 *
 * `Context::get<JwtClaims>()`으로 다운스트림 Action에서 접근합니다.
 */
struct JwtClaims {
    std::string_view sub;        ///< Subject (zero-copy, 원본 토큰 참조)
    std::string_view iss;        ///< Issuer
    std::string_view aud;        ///< Audience
    int64_t          exp{-1};    ///< Expiration timestamp (-1이면 없음)
    int64_t          iat{-1};    ///< Issued At timestamp
    int64_t          nbf{-1};    ///< Not Before timestamp

    /** @brief 토큰이 현재 시각 `now`에 유효한지 확인합니다. */
    [[nodiscard]] bool is_valid_at(int64_t now, int64_t leeway = 0) const noexcept {
        if (exp >= 0 && now > exp + leeway) return false;
        if (nbf >= 0 && now < nbf - leeway) return false;
        return true;
    }
};

// ─── JwtAuthConfig ────────────────────────────────────────────────────────────

/**
 * @brief JwtAuthAction 설정.
 */
struct JwtAuthConfig {
    int64_t  leeway_sec{0};     ///< exp/nbf 클레임 허용 오차 (초)
    size_t   cache_size{256};   ///< 검증 결과 LRU 캐시 크기 (0이면 캐시 비활성화)
    bool     require_exp{true}; ///< exp 클레임 필수 여부
    bool     require_sub{false};///< sub 클레임 필수 여부

    /** @brief Authorization 헤더 이름 (기본: "authorization"). */
    std::string_view auth_header{"authorization"};

    /** @brief Bearer 접두사 길이. `"Bearer "` = 7. */
    static constexpr size_t kBearerPrefixLen = 7;
};

// ─── JwtAuthResult ────────────────────────────────────────────────────────────

/** @brief JWT 검증 결과 코드. */
enum class JwtAuthResult : uint8_t {
    OK              = 0, ///< 검증 성공
    NoToken         = 1, ///< Authorization 헤더 없음
    InvalidFormat   = 2, ///< JWT 형식 오류 (dot 없음, base64 오류)
    Expired         = 3, ///< exp 클레임 만료
    NotYetValid     = 4, ///< nbf 클레임 미달
    SignatureInvalid = 5, ///< ITokenVerifier 서명 불일치
    MissingClaim    = 6, ///< 필수 클레임 누락
    CacheHit        = 7, ///< 캐시에서 검증 결과 재사용
};

// ─── JwtAuthAction ────────────────────────────────────────────────────────────

/**
 * @brief SIMD JWT 검증 Pipeline Action.
 *
 * ## Context 요구사항
 * - 읽기: `qbuem::Request&` (HTTP 요청 헤더에서 토큰 추출)
 * - 쓰기: `JwtClaims` (검증된 클레임을 Context에 주입)
 *
 * ## 성능 특성
 * - SIMD dot-scan + base64url 검증: ~50ns
 * - exp/nbf 클레임 파싱: ~30ns
 * - 캐시 히트 시: ~10ns (원자 읽기만)
 * - 캐시 미스 시: ITokenVerifier 실행 시간에 의존
 *
 * @tparam Msg Pipeline 메시지 타입. `qbuem::Request`를 포함하거나 itself.
 */
template <typename Msg>
class JwtAuthAction {
public:
    /**
     * @brief JwtAuthAction을 생성합니다.
     *
     * @param verifier JWT 서명 검증기 (HS256, RS256 등).
     * @param config   검증 설정.
     */
    explicit JwtAuthAction(
        middleware::ITokenVerifier& verifier,
        JwtAuthConfig              config = {}) noexcept
        : verifier_(verifier), config_(config) {
        if (config_.cache_size > 0)
            cache_ = std::make_unique<LRUCache>(config_.cache_size);
    }

    /**
     * @brief Pipeline 메시지에서 JWT를 추출하고 검증합니다.
     *
     * 검증 성공 시 `Context`에 `JwtClaims`를 주입합니다.
     * 실패 시 `ActionResult::stop()`으로 파이프라인을 중단합니다.
     *
     * @param ctx 파이프라인 컨텍스트.
     * @param msg 메시지 (HTTP Request를 포함해야 함).
     * @returns ActionResult.
     */
    Task<ActionResult> operator()(Context& ctx, Msg& msg) noexcept {
        // 토큰 추출
        auto token_sv = extract_token(msg);
        if (!token_sv) {
            co_return ActionResult::stop(
                std::make_error_code(std::errc::permission_denied));
        }

        // 현재 시각 (Unix 타임스탬프)
        const int64_t now = current_unix_time();

        // 캐시 확인
        if (cache_) {
            if (auto cached = cache_->get(*token_sv)) {
                if (cached->is_valid_at(now, config_.leeway_sec)) {
                    ctx = ctx.put(*cached);
                    stats_.cache_hits.fetch_add(1, std::memory_order_relaxed);
                    co_return ActionResult::next();
                }
            }
        }

        // SIMD 파싱
        auto view = parser_.parse(*token_sv);
        if (!view) {
            stats_.format_errors.fetch_add(1, std::memory_order_relaxed);
            co_return ActionResult::stop(
                std::make_error_code(std::errc::invalid_argument));
        }

        // exp 클레임 확인
        if (config_.require_exp) {
            if (view->is_expired(now, config_.leeway_sec)) {
                stats_.expired.fetch_add(1, std::memory_order_relaxed);
                co_return ActionResult::stop(
                    std::make_error_code(std::errc::timed_out));
            }
        }

        // 서명 검증 (ITokenVerifier 위임)
        auto claims_opt = verifier_.verify(*token_sv);
        if (!claims_opt) {
            stats_.sig_failures.fetch_add(1, std::memory_order_relaxed);
            co_return ActionResult::stop(
                std::make_error_code(std::errc::permission_denied));
        }

        // JwtClaims 구성 (zero-copy: SIMDJwtParser 뷰 우선)
        JwtClaims claims;
        auto sub = view->claim("sub");
        claims.sub = sub.value_or(std::string_view{claims_opt->subject});
        auto iss = view->claim("iss");
        claims.iss = iss.value_or(std::string_view{claims_opt->issuer});
        auto aud = view->claim("aud");
        claims.aud = aud.value_or(std::string_view{claims_opt->audience});
        claims.exp = view->claim_int("exp").value_or(claims_opt->exp);
        claims.iat = view->claim_int("iat").value_or(-1);
        claims.nbf = view->claim_int("nbf").value_or(claims_opt->nbf);

        // 필수 클레임 확인
        if (config_.require_sub && claims.sub.empty()) {
            co_return ActionResult::stop(
                std::make_error_code(std::errc::invalid_argument));
        }

        // 캐시 갱신
        if (cache_) {
            cache_->put(*token_sv, claims);
        }

        // Context에 주입
        ctx = ctx.put(claims);
        stats_.success.fetch_add(1, std::memory_order_relaxed);
        co_return ActionResult::next();
    }

    // ── 통계 ─────────────────────────────────────────────────────────────

    struct Stats {
        std::atomic<uint64_t> success{0};
        std::atomic<uint64_t> cache_hits{0};
        std::atomic<uint64_t> format_errors{0};
        std::atomic<uint64_t> expired{0};
        std::atomic<uint64_t> sig_failures{0};
    };

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    // ── 토큰 추출 ─────────────────────────────────────────────────────────

    /**
     * @brief 메시지에서 Bearer 토큰을 추출합니다.
     *
     * 메시지가 `Request` 타입이면 Authorization 헤더에서,
     * 아니면 string_view로 직접 접근을 시도합니다.
     */
    std::optional<std::string_view> extract_token(const Msg& msg) const noexcept {
        // Duck typing: msg.header(name) 또는 msg.token()
        if constexpr (requires { msg.header(config_.auth_header); }) {
            std::string_view auth = msg.header(config_.auth_header);
            if (auth.size() <= JwtAuthConfig::kBearerPrefixLen) return std::nullopt;
            // "Bearer " 접두사 확인 (대소문자 무감)
            static constexpr char kBearer[] = "bearer ";
            for (size_t i = 0; i < JwtAuthConfig::kBearerPrefixLen; ++i) {
                char c = auth[i];
                if (c >= 'A' && c <= 'Z') c |= 0x20; // tolower
                if (c != kBearer[i]) return std::nullopt;
            }
            return auth.substr(JwtAuthConfig::kBearerPrefixLen);
        } else if constexpr (requires { std::string_view{msg}; }) {
            return std::string_view{msg};
        }
        return std::nullopt;
    }

    // ── 간단한 시간 조회 ──────────────────────────────────────────────────
    static int64_t current_unix_time() noexcept {
        struct timespec ts{};
        ::clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<int64_t>(ts.tv_sec);
    }

    // ── 초간단 LRU 캐시 (고정 크기, clock 알고리즘) ───────────────────────
    struct CacheEntry {
        uint64_t  key_hash{0};    ///< 서명 파트 64-bit 해시
        JwtClaims claims{};
        bool      valid{false};
    };

    class LRUCache {
    public:
        explicit LRUCache(size_t cap) : entries_(cap) {}

        std::optional<JwtClaims> get(std::string_view token) const noexcept {
            uint64_t h = hash(token);
            size_t   idx = h % entries_.size();
            const auto& e = entries_[idx];
            if (e.valid && e.key_hash == h) return e.claims;
            return std::nullopt;
        }

        void put(std::string_view token, const JwtClaims& claims) noexcept {
            uint64_t h = hash(token);
            size_t   idx = h % entries_.size();
            entries_[idx] = {h, claims, true};
        }

    private:
        // FNV-1a 64-bit 해시 (서명 파트 기준)
        static uint64_t hash(std::string_view s) noexcept {
            uint64_t h = 0xcbf29ce484222325ULL;
            for (unsigned char c : s) {
                h ^= c;
                h *= 0x100000001b3ULL;
            }
            return h;
        }

        std::vector<CacheEntry> entries_;
    };

    // ── 데이터 멤버 ──────────────────────────────────────────────────────
    middleware::ITokenVerifier& verifier_;
    JwtAuthConfig               config_;
    SIMDJwtParser               parser_;
    std::unique_ptr<LRUCache>   cache_;
    mutable Stats               stats_;
};

} // namespace qbuem::security

/** @} */
