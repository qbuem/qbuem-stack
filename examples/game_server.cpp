/**
 * @file examples/game_server.cpp
 * @brief qbuem-stack 종합 게임 서버 예제 — 실시간 멀티플레이어 턴제 배틀
 *
 * ## 시스템 구조
 *
 *   [HTTP 클라이언트]
 *       │ POST /api/v1/rooms                    방 생성
 *       │ GET  /api/v1/rooms                    방 목록 조회
 *       │ POST /api/v1/rooms/:id/join           방 참가 (최대 2명)
 *       │ POST /api/v1/rooms/:id/action         게임 액션 제출
 *       │ GET  /api/v1/rooms/:id/events         방별 SSE 실시간 이벤트
 *       │ GET  /api/v1/leaderboard              플레이어 랭킹
 *       │ GET  /api/v1/stats                    서버 통계
 *       │ POST /api/v1/admin/scale              워커 스케일 조정
 *       │ POST /api/v1/admin/toggle             스테이지 활성/비활성
 *       │ GET  /health                          헬스체크
 *       ▼
 *   [App 미들웨어 체인]
 *       ├─ CORS          (allow_origin=*)
 *       ├─ RateLimit     (60 req/s, burst=20)   ← 치트 방지
 *       ├─ RequestID     (X-Request-ID 헤더)
 *       ├─ HSTS          (max_age=31536000)
 *       └─ BearerAuth    (GameKeyVerifier → player_id 추출)
 *
 *   ┌── [StaticPipeline: 게임 액션 처리 체인] ──────────────────────────────┐
 *   │   Action<GameAction, ValidatedAction>  validate  {min=1, max=4, auto} │
 *   │   Action<ValidatedAction, StateUpdate> apply     {min=2, max=8, auto} │
 *   │   Action<StateUpdate, GameEvent>       finalize  {min=1, max=4, slo}  │
 *   └───────────────────────────────────────────────────────────────────────┘
 *       │ ContextualItem<GameEvent> (Context 보존 브릿지 코루틴)
 *       ▼
 *   ┌── [DynamicPipeline<GameEvent>] ───────────────────────────────────────┐
 *   │   Stage "persist"  (이벤트 히스토리 저장)   ← hot_swap 가능          │
 *   │   Stage "replay"   (리플레이 데이터 적재, set_enabled 가능)           │
 *   │   Stage "notify"   (ResponseChannel + MessageBus 발행)               │
 *   └───────────────────────────────────────────────────────────────────────┘
 *       │
 *       ├─ ResponseChannel → HTTP 핸들러 co_await → JSON 응답
 *       └─ MessageBus
 *           ├─ "room.{room_id}" → 방별 SSE 스트리밍
 *           └─ "leaderboard"   → 게임 종료 시 랭킹 갱신 SSE
 *
 *   [자동 스케일러]
 *       └─ 500ms 주기로 validate/apply 큐 깊이 감시 → 부하 시 워커 추가
 *
 * ## 게임 규칙
 *   - 방당 최대 2명, 선착순 참가, 턴제 진행 (HOST가 먼저)
 *   - 각 플레이어 시작 HP: 100
 *   - attack  : power 1~10, damage = power × 4 (방어 중이면 50% 감소)
 *   - defend  : 이번 턴 방어 준비 → 다음 피격 데미지 50% 감소
 *   - special : power 1~10, damage = power × 7, 쿨다운 3턴
 *   - 상대 HP가 0 이하 → 승패 확정, 방 상태 "finished"
 *
 * ## 테스트 커맨드
 *   # 방 생성
 *   curl -H 'Authorization: Bearer game-key-alice' \
 *        -X POST http://localhost:8080/api/v1/rooms \
 *        -d '{"room_name":"arena-1"}'
 *
 *   # 방 참가 (room_id=1)
 *   curl -H 'Authorization: Bearer game-key-bob' \
 *        -X POST http://localhost:8080/api/v1/rooms/1/join
 *
 *   # 공격 액션
 *   curl -H 'Authorization: Bearer game-key-alice' \
 *        -X POST http://localhost:8080/api/v1/rooms/1/action \
 *        -d '{"action":"attack","power":8}'
 *
 *   # 방어 액션
 *   curl -H 'Authorization: Bearer game-key-bob' \
 *        -X POST http://localhost:8080/api/v1/rooms/1/action \
 *        -d '{"action":"defend","power":0}'
 *
 *   # 스페셜 스킬
 *   curl -H 'Authorization: Bearer game-key-alice' \
 *        -X POST http://localhost:8080/api/v1/rooms/1/action \
 *        -d '{"action":"special","power":10}'
 *
 *   # SSE 이벤트 스트리밍
 *   curl -N http://localhost:8080/api/v1/rooms/1/events
 *
 *   # 리더보드
 *   curl http://localhost:8080/api/v1/leaderboard
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

#include <array>
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
// §1. 게임 도메인 타입
// ─────────────────────────────────────────────────────────────────────────────

/// 게임 액션 유형
enum class ActionType : uint8_t {
    Attack  = 0,   ///< 공격 — damage = power × 4
    Defend  = 1,   ///< 방어 — 다음 피격 데미지 50% 감소
    Special = 2,   ///< 스페셜 — damage = power × 7, 쿨다운 3턴
};

/// HTTP 바디에서 파싱된 게임 액션 (Pipeline 입력 타입).
struct GameAction {
    uint64_t    req_id    = 0;       ///< 요청 고유 번호 (Context 전파)
    std::string player_id;           ///< 액션을 수행하는 플레이어
    std::string room_id;             ///< 대상 방 ID
    ActionType  type      = ActionType::Attack;
    int         power     = 5;       ///< 1~10

    static std::optional<GameAction> from_json(std::string_view body,
                                               std::string_view player,
                                               std::string_view room) {
        try {
            auto g = qbuem::fuse<GameAction>(std::string(body));
            g.player_id = player;
            g.room_id   = room;
            if (g.power < 1 || g.power > 10) g.power = 5;
            return g;
        } catch (...) { return std::nullopt; }
    }
};

/// Nexus Fusion ADL hook — "action" 문자열 → ActionType 변환.
/// qbuem::fuse<GameAction>() 호출 시 사용 (zero-tape 직접 파싱).
inline void nexus_pulse(std::string_view key, const char*& p, const char* end,
                        GameAction& g) {
    using namespace qbuem::json::detail;
    switch (fnv1a_hash(key)) {
        case fnv1a_hash_ce("power"):   from_json_direct(p, end, g.power);   break;
        case fnv1a_hash_ce("action"): {
            std::string s; from_json_direct(p, end, s);
            if      (s == "defend")  g.type = ActionType::Defend;
            else if (s == "special") g.type = ActionType::Special;
            else                     g.type = ActionType::Attack;
            break;
        }
        default: skip_direct(p, end); break;
    }
}

/// [Static Stage 1] 유효성 검증 결과.
struct ValidatedAction {
    GameAction  action;
    bool        valid     = true;
    std::string error;
    int         base_dmg  = 0;   ///< 계산된 기본 데미지
};

/// [Static Stage 2] 게임 상태 적용 결과.
struct StateUpdate {
    ValidatedAction validated;
    std::string     attacker_id;
    std::string     defender_id;
    int             damage_dealt  = 0;
    int             attacker_hp   = 0;
    int             defender_hp   = 0;
    bool            game_over     = false;
    std::string     winner_id;
};

/// [Static Stage 3] 최종 게임 이벤트 (Static Pipeline 출력 + DynamicPipeline 입력).
struct GameEvent {
    uint64_t    event_id    = 0;
    std::string room_id;
    std::string attacker;
    std::string defender;
    std::string action;         ///< "attack" | "defend" | "special"
    int         power       = 0;
    int         damage      = 0;
    int         attacker_hp = 0;
    int         defender_hp = 0;
    bool        game_over   = false;
    std::string winner;
    std::string timestamp;
    bool        success     = true;
    std::string message;
};
QBUEM_JSON_FIELDS(GameEvent, event_id, room_id, attacker, defender, action,
                  power, damage, attacker_hp, defender_hp,
                  game_over, winner, timestamp, success, message)

// ─────────────────────────────────────────────────────────────────────────────
// §2. HTTP DTO (qbuem-json Nexus 엔진 직렬화/역직렬화)
// ─────────────────────────────────────────────────────────────────────────────

/// POST /api/v1/rooms 요청.
struct CreateRoomRequest {
    std::string room_name;

    static std::optional<CreateRoomRequest> from_json(std::string_view body) {
        try {
            auto r = qbuem::fuse<CreateRoomRequest>(std::string(body));
            if (r.room_name.empty()) r.room_name = "untitled";
            return r;
        } catch (...) {
            return CreateRoomRequest{"untitled"};
        }
    }
};
QBUEM_JSON_FIELDS(CreateRoomRequest, room_name)

/// GET /api/v1/rooms 응답 — 방 정보.
struct RoomInfo {
    uint64_t                 room_id     = 0;
    std::string              room_name;
    std::vector<std::string> players;
    std::string              phase;        ///< "waiting" | "playing" | "finished"
    std::string              current_turn; ///< 현재 턴 플레이어 ID
    std::string              winner;
};
QBUEM_JSON_FIELDS(RoomInfo, room_id, room_name, players, phase, current_turn, winner)

/// GET /api/v1/leaderboard 응답 항목.
struct PlayerScore {
    std::string player_id;
    int         wins         = 0;
    int         losses       = 0;
    int         total_damage = 0;
    double      win_rate     = 0.0;
};
QBUEM_JSON_FIELDS(PlayerScore, player_id, wins, losses, total_damage, win_rate)

/// POST /api/v1/admin/scale 요청.
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

/// POST /api/v1/admin/toggle 요청.
struct ToggleRequest {
    std::string stage;
    bool        enabled = true;

    static std::optional<ToggleRequest> from_json(std::string_view body) {
        try {
            auto r = qbuem::fuse<ToggleRequest>(std::string(body));
            if (r.stage.empty()) return std::nullopt;
            return r;
        } catch (...) { return std::nullopt; }
    }
};
QBUEM_JSON_FIELDS(ToggleRequest, stage, enabled)

/// 에러 응답.
struct ErrorResponse { std::string error; };
QBUEM_JSON_FIELDS(ErrorResponse, error)

/// 성공 메시지 응답.
struct OkResponse { std::string message; };
QBUEM_JSON_FIELDS(OkResponse, message)

/// GET /api/v1/stats 응답.
struct StatsResponse {
    uint64_t    total_actions  = 0;
    uint64_t    total_games    = 0;
    uint64_t    active_rooms   = 0;
    uint64_t    finished_rooms = 0;
    bool        validate_empty = false;
    bool        apply_empty    = false;
    int         dyn_stages     = 0;
    bool        auto_scale     = false;
};
QBUEM_JSON_FIELDS(StatsResponse, total_actions, total_games, active_rooms,
                  finished_rooms, validate_empty, apply_empty, dyn_stages, auto_scale)

/// SSE 연결 확인 이벤트.
struct SseConnectedEvent { std::string message; std::string topic; };
QBUEM_JSON_FIELDS(SseConnectedEvent, message, topic)

/// Scale/Toggle 응답.
struct ScaleResponse { std::string stage; int workers = 0; std::string message; };
QBUEM_JSON_FIELDS(ScaleResponse, stage, workers, message)

struct ToggleResponse { std::string stage; bool enabled = false; bool success = false; };
QBUEM_JSON_FIELDS(ToggleResponse, stage, enabled, success)

// ─────────────────────────────────────────────────────────────────────────────
// §3. Context 태그
// ─────────────────────────────────────────────────────────────────────────────

/// HTTP 응답 채널: DynamicPipeline 마지막 Stage에서 여기에 씁니다.
struct ResponseChannel {
    std::shared_ptr<AsyncChannel<GameEvent>> ch;
};

// ─────────────────────────────────────────────────────────────────────────────
// §4. 게임 상태 저장소 (thread-safe 인메모리)
// ─────────────────────────────────────────────────────────────────────────────

/// 플레이어 1명의 현재 상태 (방 내).
struct PlayerState {
    int  hp               = 100;
    bool is_defending     = false;   ///< 이번 턴 방어 → 피격 시 50% 감소
    int  special_cooldown = 0;       ///< 남은 쿨다운 (0이면 사용 가능)
};

/// 방(Room) 상태 머신.
struct RoomState {
    uint64_t    room_id     = 0;
    std::string room_name;
    std::string player_ids[2];       ///< [0]=호스트, [1]=참가자
    PlayerState player_states[2];    ///< player_ids 와 1:1 대응
    int         player_count = 0;
    int         current_turn = 0;    ///< 0 or 1 — 현재 액션 수행 플레이어 인덱스
    enum class Phase { Waiting, Playing, Finished } phase = Phase::Waiting;
    std::string winner_id;
    uint64_t    event_counter = 0;   ///< 방 내 이벤트 순서 번호
};

/// 플레이어 누적 전적.
struct PlayerRecord {
    int    wins         = 0;
    int    losses       = 0;
    int    total_damage = 0;
};

/// 게임 이벤트 히스토리 레코드.
struct EventRecord {
    uint64_t event_id = 0;
    GameEvent event;
};

class GameRegistry {
public:
    // ── 방 관리 ────────────────────────────────────────────────────────────

    RoomInfo create_room(std::string_view name) {
        std::lock_guard lock(mtx_);
        RoomState r;
        r.room_id   = ++next_room_id_;
        r.room_name = name;
        rooms_[r.room_id] = r;
        return to_info(r);
    }

    /// 방 참가. 이미 2명이거나 finished 이면 빈 optional.
    std::optional<RoomInfo> join_room(uint64_t room_id, std::string_view player_id) {
        std::lock_guard lock(mtx_);
        auto it = rooms_.find(room_id);
        if (it == rooms_.end()) return std::nullopt;
        auto& r = it->second;

        // 이미 참가한 플레이어는 그냥 통과
        for (int i = 0; i < r.player_count; ++i)
            if (r.player_ids[i] == player_id) return to_info(r);

        if (r.player_count >= 2 || r.phase == RoomState::Phase::Finished)
            return std::nullopt;

        r.player_ids[r.player_count] = player_id;
        r.player_states[r.player_count] = PlayerState{};
        ++r.player_count;

        if (r.player_count == 2)
            r.phase = RoomState::Phase::Playing;

        return to_info(r);
    }

    std::vector<RoomInfo> list_rooms() const {
        std::lock_guard lock(mtx_);
        std::vector<RoomInfo> result;
        for (auto& [_, r] : rooms_)
            result.push_back(to_info(r));
        return result;
    }

    std::optional<RoomInfo> get_room(uint64_t id) const {
        std::lock_guard lock(mtx_);
        auto it = rooms_.find(id);
        if (it == rooms_.end()) return std::nullopt;
        return to_info(it->second);
    }

    // ── 게임 액션 적용 ─────────────────────────────────────────────────────

    /// 검증 + 상태 적용 — 성공 시 StateUpdate 반환.
    std::optional<StateUpdate> apply_action(const ValidatedAction& va) {
        std::lock_guard lock(mtx_);
        auto it = rooms_.find(0); // room_id 검색
        {
            uint64_t rid = 0;
            try { rid = std::stoull(va.action.room_id); } catch (...) {}
            it = rooms_.find(rid);
        }
        if (it == rooms_.end()) return std::nullopt;
        auto& r = it->second;

        if (r.phase != RoomState::Phase::Playing) return std::nullopt;

        // 현재 턴 플레이어 확인
        int atk_idx = r.current_turn;
        if (r.player_ids[atk_idx] != va.action.player_id) return std::nullopt;
        int def_idx = 1 - atk_idx;

        auto& atk = r.player_states[atk_idx];
        auto& def = r.player_states[def_idx];

        StateUpdate su;
        su.validated   = va;
        su.attacker_id = r.player_ids[atk_idx];
        su.defender_id = r.player_ids[def_idx];

        // 스페셜 쿨다운 감소 (매 턴)
        if (atk.special_cooldown > 0) --atk.special_cooldown;

        // 데미지 계산
        int dmg = 0;
        if (va.action.type == ActionType::Defend) {
            atk.is_defending = true;
            dmg = 0;
        } else {
            dmg = va.base_dmg;
            if (def.is_defending) dmg /= 2;   // 방어 중: 50% 감소
            if (va.action.type == ActionType::Special)
                atk.special_cooldown = 3;      // 쿨다운 설정
            def.hp -= dmg;
        }

        // 방어 상태 리셋 (피격 후)
        if (va.action.type != ActionType::Defend)
            def.is_defending = false;

        su.damage_dealt = dmg;
        su.attacker_hp  = atk.hp;
        su.defender_hp  = std::max(0, def.hp);
        def.hp          = su.defender_hp;

        // 승패 판정
        if (def.hp <= 0) {
            r.phase     = RoomState::Phase::Finished;
            r.winner_id = su.attacker_id;
            su.game_over = true;
            su.winner_id = su.attacker_id;
            // 전적 업데이트
            records_[su.attacker_id].wins++;
            records_[su.attacker_id].total_damage += dmg;
            records_[su.defender_id].losses++;
            ++finished_rooms_;
        } else {
            records_[su.attacker_id].total_damage += dmg;
            // 턴 전환
            r.current_turn = def_idx;
        }

        ++r.event_counter;
        return su;
    }

    // ── 히스토리 ───────────────────────────────────────────────────────────

    void save_event(const GameEvent& ev) {
        std::lock_guard lock(mtx_);
        history_.push_back({ev.event_id, ev});
    }

    std::vector<GameEvent> room_history(uint64_t room_id, size_t limit = 50) const {
        std::lock_guard lock(mtx_);
        std::vector<GameEvent> result;
        for (auto it = history_.rbegin(); it != history_.rend() && result.size() < limit; ++it) {
            uint64_t rid = 0;
            try { rid = std::stoull(it->event.room_id); } catch (...) {}
            if (rid == room_id) result.push_back(it->event);
        }
        std::reverse(result.begin(), result.end());
        return result;
    }

    // ── 리더보드 ───────────────────────────────────────────────────────────

    std::vector<PlayerScore> leaderboard(size_t limit = 20) const {
        std::lock_guard lock(mtx_);
        std::vector<PlayerScore> scores;
        for (auto& [pid, rec] : records_) {
            int total = rec.wins + rec.losses;
            scores.push_back(PlayerScore{
                pid, rec.wins, rec.losses, rec.total_damage,
                total > 0 ? static_cast<double>(rec.wins) / total : 0.0
            });
        }
        std::sort(scores.begin(), scores.end(),
            [](const PlayerScore& a, const PlayerScore& b) {
                if (a.wins != b.wins) return a.wins > b.wins;
                return a.total_damage > b.total_damage;
            });
        if (scores.size() > limit) scores.resize(limit);
        return scores;
    }

    // ── 통계 ───────────────────────────────────────────────────────────────

    size_t room_count() const {
        std::lock_guard lock(mtx_);
        return rooms_.size();
    }

    uint64_t finished_rooms() const { return finished_rooms_.load(); }

private:
    static std::string now_str() {
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
        return buf;
    }

    static RoomInfo to_info(const RoomState& r) {
        RoomInfo info;
        info.room_id  = r.room_id;
        info.room_name = r.room_name;
        for (int i = 0; i < r.player_count; ++i)
            info.players.push_back(r.player_ids[i]);
        switch (r.phase) {
            case RoomState::Phase::Waiting:  info.phase = "waiting";  break;
            case RoomState::Phase::Playing:  info.phase = "playing";  break;
            case RoomState::Phase::Finished: info.phase = "finished"; break;
        }
        if (r.phase == RoomState::Phase::Playing && r.player_count == 2)
            info.current_turn = r.player_ids[r.current_turn];
        info.winner = r.winner_id;
        return info;
    }

    mutable std::mutex                              mtx_;
    std::unordered_map<uint64_t, RoomState>         rooms_;
    std::unordered_map<std::string, PlayerRecord>   records_;
    std::vector<EventRecord>                        history_;
    uint64_t                                        next_room_id_{0};
    std::atomic<uint64_t>                           finished_rooms_{0};
};

// ─────────────────────────────────────────────────────────────────────────────
// §5. 인증 토큰 검증기 (ITokenVerifier 구현)
// ─────────────────────────────────────────────────────────────────────────────

class GameKeyVerifier : public ITokenVerifier {
public:
    std::optional<TokenClaims> verify(std::string_view token) noexcept override {
        // 데모용: "game-key-{player_id}" 형식
        // game-key-alice, game-key-bob, game-key-charlie, game-key-dave
        constexpr std::string_view prefix = "game-key-";
        if (token.substr(0, prefix.size()) != prefix) return std::nullopt;

        auto player_id = std::string(token.substr(prefix.size()));
        if (player_id.empty()) return std::nullopt;

        return TokenClaims{
            .subject  = player_id,
            .issuer   = "game-server",
            .audience = "game-api",
            .exp      = 9'999'999'999L,
        };
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// §6. Static Pipeline 스테이지 함수
// ─────────────────────────────────────────────────────────────────────────────

/// Stage 1: 유효성 검증 — 방 존재, 플레이어 턴, 쿨다운 체크
static Task<Result<ValidatedAction>> stage_validate(GameAction action, ActionEnv env) {
    ValidatedAction v;
    v.action = action;

    // 기본 데미지 사전 계산 (apply stage에서 레지스트리 참조 없이 사용)
    switch (action.type) {
        case ActionType::Attack:  v.base_dmg = action.power * 4; break;
        case ActionType::Defend:  v.base_dmg = 0;                break;
        case ActionType::Special: v.base_dmg = action.power * 7; break;
    }

    const char* action_str = action.type == ActionType::Attack  ? "ATTACK"  :
                             action.type == ActionType::Defend  ? "DEFEND"  : "SPECIAL";

    std::printf("  [validate #%02llu] worker=%zu player=%s room=%s action=%s power=%d base_dmg=%d\n",
        static_cast<unsigned long long>(action.req_id),
        env.worker_idx,
        action.player_id.c_str(),
        action.room_id.c_str(),
        action_str,
        action.power,
        v.base_dmg);

    co_return v;
}

/// Stage 2: 게임 상태 적용 — GameRegistry에 상태 변화 반영
static Task<Result<StateUpdate>>
stage_apply(ValidatedAction va, ActionEnv env,
            std::shared_ptr<GameRegistry> reg) {
    // 실제 시스템: 여기서 레지스트리를 업데이트하고 결과를 반환
    auto su_opt = reg->apply_action(va);
    if (!su_opt) {
        // 액션 적용 실패 (턴 불일치, 방 없음 등) → 오류 StateUpdate 생성
        StateUpdate su;
        su.validated = va;
        su.validated.valid = false;
        su.validated.error = "invalid action: wrong turn or room not playing";
        std::printf("  [apply    #%02llu] worker=%zu REJECTED: %s\n",
            static_cast<unsigned long long>(va.action.req_id),
            env.worker_idx, su.validated.error.c_str());
        co_return su;
    }

    auto& su = *su_opt;
    std::printf("  [apply    #%02llu] worker=%zu %s→%s dmg=%d atk_hp=%d def_hp=%d %s\n",
        static_cast<unsigned long long>(va.action.req_id),
        env.worker_idx,
        su.attacker_id.c_str(),
        su.defender_id.c_str(),
        su.damage_dealt,
        su.attacker_hp,
        su.defender_hp,
        su.game_over ? "GAME_OVER!" : "");
    co_return su;
}

/// Stage 3: 게임 이벤트 생성 — StateUpdate → GameEvent (SLO 추적 대상)
static Task<Result<GameEvent>>
stage_finalize(StateUpdate su, ActionEnv env,
               std::atomic<uint64_t>& event_counter) {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    char tsbuf[32];
    std::strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));

    const char* action_str =
        su.validated.action.type == ActionType::Attack  ? "attack"  :
        su.validated.action.type == ActionType::Defend  ? "defend"  : "special";

    GameEvent ev;
    ev.event_id    = event_counter.fetch_add(1, std::memory_order_relaxed);
    ev.room_id     = su.validated.action.room_id;
    ev.attacker    = su.attacker_id;
    ev.defender    = su.defender_id;
    ev.action      = action_str;
    ev.power       = su.validated.action.power;
    ev.damage      = su.damage_dealt;
    ev.attacker_hp = su.attacker_hp;
    ev.defender_hp = su.defender_hp;
    ev.game_over   = su.game_over;
    ev.winner      = su.winner_id;
    ev.timestamp   = tsbuf;
    ev.success     = su.validated.valid;
    ev.message     = su.validated.valid
                   ? (su.game_over ? "Game over! " + su.winner_id + " wins!" : "Action applied")
                   : su.validated.error;

    std::printf("  [finalize #%02llu] worker=%zu event_id=%llu %s\n",
        static_cast<unsigned long long>(su.validated.action.req_id),
        env.worker_idx,
        static_cast<unsigned long long>(ev.event_id),
        ev.success ? "OK" : ev.message.c_str());

    co_return ev;
}

// ─────────────────────────────────────────────────────────────────────────────
// §7. Dynamic Pipeline 스테이지 함수
// ─────────────────────────────────────────────────────────────────────────────

/// Dynamic Stage "persist": 이벤트 히스토리 저장
static auto make_persist_stage(std::shared_ptr<GameRegistry> reg) {
    return [reg](GameEvent ev, ActionEnv env) -> Task<Result<GameEvent>> {
        reg->save_event(ev);
        std::printf("  [persist  #%02llu] worker=%zu room=%s saved\n",
            static_cast<unsigned long long>(ev.event_id),
            env.worker_idx, ev.room_id.c_str());
        co_return ev;
    };
}

/// Dynamic Stage "replay": 리플레이 버퍼 적재 (set_enabled 시연 — 비활성 시 패스스루)
static Task<Result<GameEvent>> stage_replay(GameEvent ev, ActionEnv env) {
    // 실제 구현: 리플레이 버퍼에 기록 (여기서는 로그만)
    std::printf("  [replay   #%02llu] worker=%zu [recorded for replay]\n",
        static_cast<unsigned long long>(ev.event_id), env.worker_idx);
    co_return ev;
}

/// Dynamic Stage "notify": ResponseChannel 응답 + MessageBus 팬아웃
static auto make_notify_stage(std::shared_ptr<MessageBus> bus,
                               std::atomic<uint64_t>& total_actions,
                               std::atomic<uint64_t>& total_games) {
    return [bus, &total_actions, &total_games]
           (GameEvent ev, ActionEnv env) -> Task<Result<GameEvent>> {

        // ── 1) ResponseChannel에 HTTP 응답 전송 ───────────────────────────
        if (auto* rch = env.ctx.get_ptr<ResponseChannel>()) {
            rch->ch->try_send(ev);
        }

        // ── 2) 통계 업데이트 ──────────────────────────────────────────────
        total_actions.fetch_add(1, std::memory_order_relaxed);
        if (ev.game_over)
            total_games.fetch_add(1, std::memory_order_relaxed);

        // ── 3) MessageBus 발행 (방별 + 전체 SSE 팬아웃) ──────────────────
        std::string room_topic = "room." + ev.room_id;
        bus->try_publish(room_topic, ev);

        if (ev.game_over) {
            bus->try_publish("leaderboard", ev);  // 리더보드 갱신 트리거
        }

        std::printf("  [notify   #%02llu] worker=%zu → 응답+SSE 발행 (topic=%s)\n",
            static_cast<unsigned long long>(ev.event_id),
            env.worker_idx, room_topic.c_str());

        co_return ev;
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// §8. GameServer — 전체 상태 보유 구조체
// ─────────────────────────────────────────────────────────────────────────────

struct GameServer {
    // ── 인프라 ────────────────────────────────────────────────────────────
    std::shared_ptr<GameRegistry> reg = std::make_shared<GameRegistry>();
    std::shared_ptr<MessageBus>   bus = std::make_shared<MessageBus>();

    // ── Static Pipeline Actions ──────────────────────────────────────────
    using VAction = Action<GameAction, ValidatedAction>;
    using AAction = Action<ValidatedAction, StateUpdate>;
    using FAction = Action<StateUpdate, GameEvent>;

    std::shared_ptr<VAction> validate_act;
    std::shared_ptr<AAction> apply_act;
    std::shared_ptr<FAction> finalize_act;

    // Static Pipeline 최종 출력 채널
    std::shared_ptr<AsyncChannel<ContextualItem<GameEvent>>> finalize_out;

    // ── Dynamic Pipeline ──────────────────────────────────────────────────
    std::shared_ptr<DynamicPipeline<GameEvent>> dyn_pipe;

    // ── 통계 ─────────────────────────────────────────────────────────────
    std::atomic<uint64_t> total_actions{0};
    std::atomic<uint64_t> total_games{0};
    std::atomic<uint64_t> req_counter{0};
    std::atomic<uint64_t> event_counter{0};
    std::atomic<bool>     auto_scale_on{true};

    Dispatcher* disp = nullptr;

    void setup(Dispatcher& d) {
        disp = &d;
        bus->start(d);

        // ── Stage 함수 바인딩 (레지스트리 캡처) ──────────────────────────
        auto bound_apply = [this](ValidatedAction va, ActionEnv env) {
            return stage_apply(std::move(va), env, this->reg);
        };
        auto bound_finalize = [this](StateUpdate su, ActionEnv env) {
            return stage_finalize(std::move(su), env, this->event_counter);
        };

        // ── Static Pipeline Actions 생성 ─────────────────────────────────
        validate_act = std::make_shared<VAction>(
            stage_validate,
            VAction::Config{
                .min_workers = 1,
                .max_workers = 4,
                .channel_cap = 512,
                .auto_scale  = true,
                .slo = SloConfig{
                    .p99_target   = std::chrono::microseconds{1'000},
                    .error_budget = 0.001,
                },
            });

        apply_act = std::make_shared<AAction>(
            bound_apply,
            AAction::Config{
                .min_workers = 2,
                .max_workers = 8,
                .channel_cap = 512,
                .auto_scale  = true,
                .slo = SloConfig{
                    .p99_target   = std::chrono::microseconds{3'000},
                    .error_budget = 0.005,
                },
            });

        finalize_act = std::make_shared<FAction>(
            bound_finalize,
            FAction::Config{
                .min_workers = 1,
                .max_workers = 4,
                .channel_cap = 256,
                .auto_scale  = true,
            });

        // ── Static Actions 연결 및 시작 ──────────────────────────────────
        finalize_out = std::make_shared<AsyncChannel<ContextualItem<GameEvent>>>(512);
        finalize_act->start(d, finalize_out);
        apply_act->start(d, finalize_act->input());
        validate_act->start(d, apply_act->input());

        // ── Dynamic Pipeline 생성 ─────────────────────────────────────────
        dyn_pipe = std::make_shared<DynamicPipeline<GameEvent>>(
            DynamicPipeline<GameEvent>::Config{
                .default_channel_cap = 256,
                .default_workers     = 2,
            });

        dyn_pipe->add_stage("persist", make_persist_stage(reg));
        dyn_pipe->add_stage("replay",  stage_replay);
        dyn_pipe->add_stage("notify",  make_notify_stage(bus, total_actions, total_games));

        dyn_pipe->start(d);

        // ── Static → Dynamic 브릿지 코루틴 ───────────────────────────────
        d.spawn(run_bridge(finalize_out, dyn_pipe));

        // ── 자동 스케일러 ─────────────────────────────────────────────────
        d.spawn(run_autoscaler(this));

        std::puts("[game-server] setup: static(3 actions) + dynamic(3 stages) + autoscaler");
    }

    static Task<void> run_bridge(
        std::shared_ptr<AsyncChannel<ContextualItem<GameEvent>>> out,
        std::shared_ptr<DynamicPipeline<GameEvent>> dyn_pipe)
    {
        for (;;) {
            auto item = co_await out->recv();
            if (!item) break;
            co_await dyn_pipe->push(item->value, item->ctx);
        }
    }

    static Task<void> run_autoscaler(GameServer* s) {
        for (;;) {
            co_await qbuem::sleep(500);
            if (!s->auto_scale_on.load()) continue;

            if (s->validate_act->input()->size_approx() > 0) {
                s->validate_act->scale_out(*s->disp);
                std::puts("[autoscale] validate busy → scale_out");
            } else {
                s->validate_act->scale_in();
            }
            if (s->apply_act->input()->size_approx() > 0) {
                s->apply_act->scale_out(*s->disp);
                std::puts("[autoscale] apply busy → scale_out");
            } else {
                s->apply_act->scale_in();
            }
        }
    }

    /// 게임 액션을 파이프라인에 제출. 응답 채널 반환 (nullptr=포화).
    std::shared_ptr<AsyncChannel<GameEvent>>
    submit_action(GameAction action, Context base_ctx) {
        auto resp_ch = std::make_shared<AsyncChannel<GameEvent>>(1);
        auto ctx = base_ctx.put(ResponseChannel{resp_ch});
        action.req_id = ++req_counter;
        if (!validate_act->try_push(action, ctx))
            return nullptr;
        return resp_ch;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// §9. HTTP 핸들러
// ─────────────────────────────────────────────────────────────────────────────

/// POST /api/v1/rooms — 방 생성 (생성자는 자동 참가)
static Handler make_post_room(GameServer& s) {
    return [&s](const Request& req, Response& res) {
        auto dto = CreateRoomRequest::from_json(req.body());
        auto info = s.reg->create_room(dto ? dto->room_name : "untitled");
        // 방 생성자는 자동으로 참가
        auto creator = res.get_header("X-Auth-Sub");
        if (!creator.empty()) {
            if (auto joined = s.reg->join_room(info.room_id, std::string(creator)))
                info = *joined;
        }
        res.status(201)
           .header("Content-Type", "application/json")
           .body(qbuem::write(info));
    };
}

/// GET /api/v1/rooms — 방 목록
static Handler make_get_rooms(GameServer& s) {
    return [&s](const Request& /*req*/, Response& res) {
        auto rooms = s.reg->list_rooms();
        res.status(200)
           .header("Content-Type", "application/json")
           .body(qbuem::write(rooms));
    };
}

/// GET /api/v1/rooms/:id — 방 상세
static Handler make_get_room(GameServer& s) {
    return [&s](const Request& req, Response& res) {
        uint64_t id = 0;
        try { id = std::stoull(std::string(req.param("id"))); } catch (...) {
            res.status(400).body(qbuem::write(ErrorResponse{"invalid room id"})); return;
        }
        auto info = s.reg->get_room(id);
        if (!info) { res.status(404).body(qbuem::write(ErrorResponse{"room not found"})); return; }
        res.status(200).header("Content-Type", "application/json").body(qbuem::write(*info));
    };
}

/// POST /api/v1/rooms/:id/join — 방 참가
static Handler make_post_join(GameServer& s) {
    return [&s](const Request& req, Response& res) {
        uint64_t id = 0;
        try { id = std::stoull(std::string(req.param("id"))); } catch (...) {
            res.status(400).body(qbuem::write(ErrorResponse{"invalid room id"})); return;
        }
        auto player = res.get_header("X-Auth-Sub");
        if (player.empty()) {
            res.status(401).body(qbuem::write(ErrorResponse{"authentication required"})); return;
        }
        auto info = s.reg->join_room(id, std::string(player));
        if (!info) {
            res.status(409).body(qbuem::write(ErrorResponse{"room full or finished"})); return;
        }
        res.status(200)
           .header("Content-Type", "application/json")
           .body(qbuem::write(*info));
    };
}

/// POST /api/v1/rooms/:id/action — 게임 액션 제출 (Pipeline 처리 후 응답)
static AsyncHandler make_post_action(GameServer& s) {
    return [&s](const Request& req, Response& res) -> Task<void> {
        uint64_t room_id = 0;
        try { room_id = std::stoull(std::string(req.param("id"))); } catch (...) {
            res.status(400).header("Content-Type", "application/json")
               .body(qbuem::write(ErrorResponse{"invalid room id"}));
            co_return;
        }

        auto player = res.get_header("X-Auth-Sub");
        if (player.empty()) {
            res.status(401).header("Content-Type", "application/json")
               .body(qbuem::write(ErrorResponse{"authentication required"}));
            co_return;
        }

        auto action = GameAction::from_json(req.body(), player,
                                            std::to_string(room_id));
        if (!action) {
            res.status(400).header("Content-Type", "application/json")
               .body(qbuem::write(ErrorResponse{
                   "body required: {\"action\":\"attack|defend|special\",\"power\":1-10}"}));
            co_return;
        }

        auto ctx = Context{}
            .put(RequestId{std::string(req.header("X-Request-ID"))})
            .put(AuthSubject{std::string(player)});

        auto resp_ch = s.submit_action(*action, ctx);
        if (!resp_ch) {
            res.status(503).header("Content-Type", "application/json")
               .body(qbuem::write(ErrorResponse{"server overloaded, retry later"}));
            co_return;
        }

        auto ev = co_await resp_ch->recv();
        if (!ev) {
            res.status(500).header("Content-Type", "application/json")
               .body(qbuem::write(ErrorResponse{"internal error"}));
            co_return;
        }

        res.status(ev->success ? 200 : 422)
           .header("Content-Type", "application/json")
           .body(qbuem::write(*ev));
    };
}

/// GET /api/v1/rooms/:id/events — 방별 SSE 실시간 이벤트 스트림
static AsyncHandler make_sse_room_events(GameServer& s) {
    return [&s](const Request& req, Response& res) -> Task<void> {
        uint64_t room_id = 0;
        try { room_id = std::stoull(std::string(req.param("id"))); } catch (...) {}

        std::string topic = "room." + std::to_string(room_id);
        auto stream = s.bus->subscribe_stream<GameEvent>(topic, 64);

        SseStream sse(res);
        sse.send(qbuem::write(SseConnectedEvent{"connected", topic}), "connected");

        // 최대 100개 이벤트 수신 (또는 게임 종료 시)
        for (int i = 0; i < 100; ++i) {
            auto ev = co_await stream->recv();
            if (!ev) break;
            sse.send(qbuem::write(*ev), ev->game_over ? "game_over" : "action");
            if (ev->game_over) break;
        }
        sse.close();
    };
}

/// GET /api/v1/leaderboard — 플레이어 랭킹 SSE + JSON
static AsyncHandler make_get_leaderboard(GameServer& s) {
    return [&s](const Request& req, Response& res) -> Task<void> {
        // SSE 구독 여부 (Accept: text/event-stream)
        bool is_sse = req.header("Accept") == "text/event-stream";
        if (!is_sse) {
            auto scores = s.reg->leaderboard(20);
            res.status(200)
               .header("Content-Type", "application/json")
               .body(qbuem::write(scores));
            co_return;
        }

        // SSE 모드: 게임 종료 이벤트마다 리더보드 갱신 푸시
        auto stream = s.bus->subscribe_stream<GameEvent>("leaderboard", 16);
        SseStream sse(res);
        sse.send(qbuem::write(s.reg->leaderboard(20)), "leaderboard");

        for (int i = 0; i < 50; ++i) {
            auto ev = co_await stream->recv();
            if (!ev) break;
            sse.send(qbuem::write(s.reg->leaderboard(20)), "leaderboard");
        }
        sse.close();
    };
}

/// GET /api/v1/stats — 서버 통계
static Handler make_get_stats(GameServer& s) {
    return [&s](const Request& /*req*/, Response& res) {
        StatsResponse st;
        st.total_actions  = s.total_actions.load();
        st.total_games    = s.total_games.load();
        st.active_rooms   = s.reg->room_count() - s.reg->finished_rooms();
        st.finished_rooms = s.reg->finished_rooms();
        st.validate_empty = s.validate_act->input()->size_approx() == 0;
        st.apply_empty    = s.apply_act->input()->size_approx() == 0;
        st.dyn_stages     = static_cast<int>(s.dyn_pipe->stage_count());
        st.auto_scale     = s.auto_scale_on.load();
        res.status(200)
           .header("Content-Type", "application/json")
           .body(qbuem::write(st));
    };
}

/// POST /api/v1/admin/scale — 워커 스케일 조정
static Handler make_post_scale(GameServer& s) {
    return [&s](const Request& req, Response& res) {
        auto dto = ScaleRequest::from_json(req.body());
        if (!dto) {
            res.status(400).body(qbuem::write(ErrorResponse{"required: stage, workers"})); return;
        }
        if (dto->stage == "validate") {
            s.validate_act->scale_to(static_cast<size_t>(dto->workers), *s.disp);
        } else if (dto->stage == "apply") {
            s.apply_act->scale_to(static_cast<size_t>(dto->workers), *s.disp);
        } else if (dto->stage == "finalize") {
            s.finalize_act->scale_to(static_cast<size_t>(dto->workers), *s.disp);
        } else {
            res.status(400).body(qbuem::write(ErrorResponse{"stage: validate|apply|finalize"})); return;
        }
        res.status(200).header("Content-Type", "application/json")
           .body(qbuem::write(ScaleResponse{dto->stage, dto->workers, "scale applied"}));
    };
}

/// POST /api/v1/admin/toggle — DynamicPipeline 스테이지 활성/비활성
static Handler make_post_toggle(GameServer& s) {
    return [&s](const Request& req, Response& res) {
        auto dto = ToggleRequest::from_json(req.body());
        if (!dto) {
            res.status(400).body(qbuem::write(ErrorResponse{"required: stage, enabled"})); return;
        }
        bool ok = s.dyn_pipe->set_enabled(dto->stage, dto->enabled);
        std::printf("[toggle] stage=%s enabled=%s\n",
                    dto->stage.c_str(), dto->enabled ? "true" : "false");
        res.status(ok ? 200 : 404).header("Content-Type", "application/json")
           .body(qbuem::write(ToggleResponse{dto->stage, dto->enabled, ok}));
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// §10. 데모 시뮬레이션
// ─────────────────────────────────────────────────────────────────────────────

static Task<void> run_demo(GameServer& s, Dispatcher& /*d*/) {
    std::puts("\n╔══════════════════════════════════════════════════════════╗");
    std::puts("║       qbuem-stack 게임 서버 데모 시작                    ║");
    std::puts("╚══════════════════════════════════════════════════════════╝\n");

    co_await qbuem::sleep(200);  // 파이프라인 워커 시작 대기

    // ── Phase 1: 방 생성 및 참가 ─────────────────────────────────────────
    std::puts("─── Phase 1: 방 생성 및 플레이어 참가 ────────────────────");

    auto room1 = s.reg->create_room("arena-alpha");
    std::printf("  방 생성: room_id=%llu name=%s\n",
        static_cast<unsigned long long>(room1.room_id), room1.room_name.c_str());

    s.reg->join_room(room1.room_id, "alice");
    s.reg->join_room(room1.room_id, "bob");
    std::printf("  alice, bob 참가 완료. 게임 시작!\n\n");

    auto room2 = s.reg->create_room("arena-beta");
    s.reg->join_room(room2.room_id, "charlie");
    s.reg->join_room(room2.room_id, "dave");
    std::printf("  방 생성: room_id=%llu — charlie vs dave\n\n",
        static_cast<unsigned long long>(room2.room_id));

    // ── Phase 2: 게임 진행 (alice vs bob) ───────────────────────────────
    std::puts("─── Phase 2: alice vs bob 배틀 ────────────────────────────");

    struct DemoAction {
        std::string player;
        uint64_t    room_id;
        ActionType  type;
        int         power;
        const char* desc;
    };

    std::string r1 = std::to_string(room1.room_id);
    std::string r2 = std::to_string(room2.room_id);

    DemoAction demo_actions[] = {
        // alice vs bob — 7턴 배틀
        {"alice",   room1.room_id, ActionType::Attack,  8,  "alice 공격 (power=8, dmg=32)"},
        {"bob",     room1.room_id, ActionType::Defend,  0,  "bob 방어 준비"},
        {"alice",   room1.room_id, ActionType::Special, 7,  "alice 스페셜 (power=7, dmg=49 but bob은 방어중→24)"},
        {"bob",     room1.room_id, ActionType::Attack,  6,  "bob 공격 (power=6, dmg=24)"},
        {"alice",   room1.room_id, ActionType::Attack,  9,  "alice 공격 (power=9, dmg=36)"},
        {"bob",     room1.room_id, ActionType::Attack,  5,  "bob 공격 (power=5, dmg=20)"},
        {"alice",   room1.room_id, ActionType::Attack,  10, "alice 공격 (power=10, dmg=40)"},
        // charlie vs dave (room2)
        {"charlie", room2.room_id, ActionType::Special, 10, "charlie 스페셜 (power=10, dmg=70)"},
        {"dave",    room2.room_id, ActionType::Attack,   3, "dave 공격 (power=3, dmg=12)"},
        {"charlie", room2.room_id, ActionType::Attack,   8, "charlie 공격 (power=8, dmg=32)"},
    };

    for (size_t i = 0; i < std::size(demo_actions); ++i) {
        const auto& a = demo_actions[i];
        std::printf("\n[demo %zu] %s\n", i + 1, a.desc);

        GameAction action;
        action.player_id = a.player;
        action.room_id   = std::to_string(a.room_id);
        action.type      = a.type;
        action.power     = a.power;

        auto ctx = Context{}
            .put(RequestId{"demo-" + std::to_string(i + 1)})
            .put(AuthSubject{a.player});

        auto resp_ch = s.submit_action(action, ctx);
        if (!resp_ch) { std::puts("  → [ERROR] overloaded"); continue; }

        auto ev = co_await resp_ch->recv();
        if (!ev) { std::puts("  → [ERROR] no result"); continue; }

        std::printf("  → %s vs %s: dmg=%d atk_hp=%d def_hp=%d %s\n",
            ev->attacker.c_str(), ev->defender.c_str(),
            ev->damage, ev->attacker_hp, ev->defender_hp,
            ev->game_over ? ("★ GAME OVER! " + ev->winner + " wins!").c_str() : "");
    }

    // ── Phase 3: 스케일 시연 ─────────────────────────────────────────────
    std::puts("\n─── Phase 3: Manual Scale In/Out ───────────────────────────");
    std::printf("[demo] validate scale_out: 1→2\n");
    s.validate_act->scale_out(*s.disp);
    std::printf("[demo] apply scale_to(4): 2→4\n");
    s.apply_act->scale_to(4, *s.disp);

    // ── Phase 4: DynamicPipeline replay 스테이지 비활성화 ────────────────
    std::puts("\n─── Phase 4: stage toggle (replay OFF) ────────────────────");
    s.dyn_pipe->set_enabled("replay", false);
    std::puts("  [toggle] replay disabled (pass-through)");

    // ── Phase 5: 리더보드 출력 ────────────────────────────────────────────
    std::puts("\n─── Phase 5: 리더보드 ─────────────────────────────────────");
    co_await qbuem::sleep(300);  // 파이프라인 완료 대기
    auto scores = s.reg->leaderboard(10);
    std::printf("  %-15s %5s %5s %12s %8s\n", "Player", "Wins", "Loss", "TotalDmg", "WinRate");
    std::puts("  --------------------------------------------------");
    for (auto& sc : scores) {
        std::printf("  %-15s %5d %5d %12d %7.1f%%\n",
            sc.player_id.c_str(), sc.wins, sc.losses,
            sc.total_damage, sc.win_rate * 100.0);
    }

    // ── Phase 6: 최종 통계 ───────────────────────────────────────────────
    std::puts("\n─── Phase 6: 서버 통계 ─────────────────────────────────────");
    std::printf("  총 액션: %llu  완료 게임: %llu  방 수: %zu\n",
        static_cast<unsigned long long>(s.total_actions.load()),
        static_cast<unsigned long long>(s.total_games.load()),
        s.reg->room_count());

    std::puts("\n╔══════════════════════════════════════════════════════════╗");
    std::puts("║       데모 완료. HTTP 서버 실행 중 (포트 8080)           ║");
    std::puts("║       curl -H 'Authorization: Bearer game-key-alice' \\   ║");
    std::puts("║         -X POST http://localhost:8080/api/v1/rooms \\     ║");
    std::puts("║         -d '{\"room_name\":\"my-room\"}'                    ║");
    std::puts("╚══════════════════════════════════════════════════════════╝\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// §11. main()
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    // ── Pipeline Dispatcher (별도 스레드 풀) ─────────────────────────────
    Dispatcher pipeline_disp(4);
    std::thread pipeline_thread([&pipeline_disp] { pipeline_disp.run(); });

    // ── GameServer 초기화 ─────────────────────────────────────────────────
    GameServer server;
    server.setup(pipeline_disp);

    // ── HTTP App 초기화 ───────────────────────────────────────────────────
    App app(2);

    // ── 미들웨어 체인 ─────────────────────────────────────────────────────
    app.use(cors(CorsConfig{
        .allow_origin      = "*",
        .allow_methods     = "GET, POST, DELETE, OPTIONS",
        .allow_headers     = "Content-Type, Authorization, X-Request-ID",
        .allow_credentials = false,
    }));

    app.use(rate_limit(RateLimitConfig{
        .rate_per_sec = 60.0,    // 초당 60 요청 (치트 방지)
        .max_keys     = 10'000,
        .burst        = 20.0,
    }));

    app.use(request_id("X-Request-ID"));
    app.use(hsts(31'536'000, /*include_subdomains=*/true));

    // bearer_auth 미들웨어 — 공개(읽기전용) 경로는 인증 없이 통과
    static GameKeyVerifier verifier;
    auto auth_mw = bearer_auth(verifier);
    app.use([auth_mw](const Request& req, Response& res) -> bool {
        std::string_view path = req.path();
        // 인증 없이 접근 가능한 공개 경로
        if (path == "/health") return true;
        if (req.method() == Method::Get) {
            if (path == "/api/v1/stats"    ||
                path == "/api/v1/leaderboard" ||
                path == "/api/v1/rooms"    ||
                path.starts_with("/api/v1/rooms/"))
                return true;
        }
        return auth_mw(req, res);
    });

    // ── 라우트 등록 ───────────────────────────────────────────────────────
    app.post("/api/v1/rooms",                make_post_room(server));
    app.get ("/api/v1/rooms",                make_get_rooms(server));
    app.get ("/api/v1/rooms/:id",            make_get_room(server));
    app.post("/api/v1/rooms/:id/join",       make_post_join(server));
    app.post("/api/v1/rooms/:id/action",     make_post_action(server));
    app.get ("/api/v1/rooms/:id/events",     make_sse_room_events(server));
    app.get ("/api/v1/leaderboard",          make_get_leaderboard(server));
    app.get ("/api/v1/stats",                make_get_stats(server));
    app.post("/api/v1/admin/scale",          make_post_scale(server));
    app.post("/api/v1/admin/toggle",         make_post_toggle(server));
    app.health_check("/health");

    // ── 전역 에러 핸들러 ─────────────────────────────────────────────────
    app.on_error([](std::exception_ptr ep, const Request& req, Response& res) {
        std::string msg;
        try { std::rethrow_exception(ep); }
        catch (const std::exception& e) { msg = e.what(); }
        catch (...)                     { msg = "unknown error"; }
        std::fprintf(stderr, "[error] %s: %s\n", std::string(req.path()).c_str(), msg.c_str());
        res.status(500)
           .header("Content-Type", "application/json")
           .body(qbuem::write(ErrorResponse{msg}));
    });

    // ── 데모 시뮬레이션 실행 후 HTTP 서버 시작 ───────────────────────────
    pipeline_disp.spawn(run_demo(server, pipeline_disp));

    std::puts("[game-server] HTTP listening on :8080");
    auto listen_result = app.listen(8080);

    pipeline_disp.stop();
    pipeline_thread.join();
    return listen_result ? 0 : 1;
}
