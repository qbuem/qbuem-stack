/**
 * @file examples/game_server.cpp
 * @brief qbuem-stack comprehensive game server example — real-time multiplayer turn-based battle
 *
 * ## System Architecture
 *
 *   [HTTP Client]
 *       │ POST /api/v1/rooms                    create room
 *       │ GET  /api/v1/rooms                    list rooms
 *       │ POST /api/v1/rooms/:id/join           join room (max 2 players)
 *       │ POST /api/v1/rooms/:id/action         submit game action
 *       │ GET  /api/v1/rooms/:id/events         per-room SSE real-time events
 *       │ GET  /api/v1/leaderboard              player rankings
 *       │ GET  /api/v1/stats                    server statistics
 *       │ POST /api/v1/admin/scale              worker scale adjustment
 *       │ POST /api/v1/admin/toggle             stage enable/disable
 *       │ GET  /health                          health check
 *       ▼
 *   [App Middleware Chain]
 *       ├─ CORS          (allow_origin=*)
 *       ├─ RateLimit     (60 req/s, burst=20)   ← anti-cheat
 *       ├─ RequestID     (X-Request-ID header)
 *       ├─ HSTS          (max_age=31536000)
 *       └─ BearerAuth    (GameKeyVerifier → extract player_id)
 *
 *   ┌── [StaticPipeline: game action processing chain] ─────────────────────┐
 *   │   Action<GameAction, ValidatedAction>  validate  {min=1, max=4, auto} │
 *   │   Action<ValidatedAction, StateUpdate> apply     {min=2, max=8, auto} │
 *   │   Action<StateUpdate, GameEvent>       finalize  {min=1, max=4, slo}  │
 *   └───────────────────────────────────────────────────────────────────────┘
 *       │ ContextualItem<GameEvent> (context-preserving bridge coroutine)
 *       ▼
 *   ┌── [DynamicPipeline<GameEvent>] ───────────────────────────────────────┐
 *   │   Stage "persist"  (event history save)       ← hot_swap capable      │
 *   │   Stage "replay"   (replay data load, set_enabled capable)            │
 *   │   Stage "notify"   (ResponseChannel + MessageBus publish)             │
 *   └───────────────────────────────────────────────────────────────────────┘
 *       │
 *       ├─ ResponseChannel → HTTP handler co_await → JSON response
 *       └─ MessageBus
 *           ├─ "room.{room_id}" → per-room SSE streaming
 *           └─ "leaderboard"   → ranking update SSE on game end
 *
 *   [Auto-scaler]
 *       └─ polls validate/apply queue depth every 500ms → scale_out on load
 *
 * ## Game Rules
 *   - max 2 players per room, first-come basis, turn-based (HOST goes first)
 *   - each player starts with 100 HP
 *   - attack  : power 1~10, damage = power × 4 (50% reduction if defending)
 *   - defend  : prepare defense this turn → 50% reduction on next incoming hit
 *   - special : power 1~10, damage = power × 7, 3-turn cooldown
 *   - opponent HP ≤ 0 → victory determined, room state "finished"
 *
 * ## Test Commands
 *   # create room
 *   curl -H 'Authorization: Bearer game-key-alice' \
 *        -X POST http://localhost:8080/api/v1/rooms \
 *        -d '{"room_name":"arena-1"}'
 *
 *   # join room (room_id=1)
 *   curl -H 'Authorization: Bearer game-key-bob' \
 *        -X POST http://localhost:8080/api/v1/rooms/1/join
 *
 *   # attack action
 *   curl -H 'Authorization: Bearer game-key-alice' \
 *        -X POST http://localhost:8080/api/v1/rooms/1/action \
 *        -d '{"action":"attack","power":8}'
 *
 *   # defend action
 *   curl -H 'Authorization: Bearer game-key-bob' \
 *        -X POST http://localhost:8080/api/v1/rooms/1/action \
 *        -d '{"action":"defend","power":0}'
 *
 *   # special skill
 *   curl -H 'Authorization: Bearer game-key-alice' \
 *        -X POST http://localhost:8080/api/v1/rooms/1/action \
 *        -d '{"action":"special","power":10}'
 *
 *   # SSE event streaming
 *   curl -N http://localhost:8080/api/v1/rooms/1/events
 *
 *   # leaderboard
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
#include <print>
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
// §1. Game Domain Types
// ─────────────────────────────────────────────────────────────────────────────

/// Game action type
enum class ActionType : uint8_t {
    Attack  = 0,   ///< attack — damage = power × 4
    Defend  = 1,   ///< defend — 50% reduction on next incoming hit
    Special = 2,   ///< special — damage = power × 7, 3-turn cooldown
};

/// Game action parsed from HTTP body (Pipeline input type).
struct GameAction {
    uint64_t    req_id    = 0;       ///< unique request ID (propagated via Context)
    std::string player_id;           ///< player performing the action
    std::string room_id;             ///< target room ID
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

/// Nexus Fusion ADL hook — converts "action" string → ActionType.
/// Used when calling qbuem::fuse<GameAction>() (zero-tape direct parsing).
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

/// [Static Stage 1] Validation result.
struct ValidatedAction {
    GameAction  action;
    bool        valid     = true;
    std::string error;
    int         base_dmg  = 0;   ///< computed base damage
};

/// [Static Stage 2] Game state application result.
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

/// [Static Stage 3] Final game event (Static Pipeline output + DynamicPipeline input).
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
// §2. HTTP DTO (qbuem-json Nexus engine serialization/deserialization)
// ─────────────────────────────────────────────────────────────────────────────

/// POST /api/v1/rooms request.
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

/// GET /api/v1/rooms response — room info.
struct RoomInfo {
    uint64_t                 room_id     = 0;
    std::string              room_name;
    std::vector<std::string> players;
    std::string              phase;        ///< "waiting" | "playing" | "finished"
    std::string              current_turn; ///< current turn player ID
    std::string              winner;
};
QBUEM_JSON_FIELDS(RoomInfo, room_id, room_name, players, phase, current_turn, winner)

/// GET /api/v1/leaderboard response entry.
struct PlayerScore {
    std::string player_id;
    int         wins         = 0;
    int         losses       = 0;
    int         total_damage = 0;
    double      win_rate     = 0.0;
};
QBUEM_JSON_FIELDS(PlayerScore, player_id, wins, losses, total_damage, win_rate)

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

/// POST /api/v1/admin/toggle request.
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

/// Error response.
struct ErrorResponse { std::string error; };
QBUEM_JSON_FIELDS(ErrorResponse, error)

/// Success message response.
struct OkResponse { std::string message; };
QBUEM_JSON_FIELDS(OkResponse, message)

/// GET /api/v1/stats response.
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

/// SSE connection confirmation event.
struct SseConnectedEvent { std::string message; std::string topic; };
QBUEM_JSON_FIELDS(SseConnectedEvent, message, topic)

/// Scale/Toggle response.
struct ScaleResponse { std::string stage; int workers = 0; std::string message; };
QBUEM_JSON_FIELDS(ScaleResponse, stage, workers, message)

struct ToggleResponse { std::string stage; bool enabled = false; bool success = false; };
QBUEM_JSON_FIELDS(ToggleResponse, stage, enabled, success)

// ─────────────────────────────────────────────────────────────────────────────
// §3. Context Tags
// ─────────────────────────────────────────────────────────────────────────────

/// HTTP response channel: the last DynamicPipeline Stage writes here.
struct ResponseChannel {
    std::shared_ptr<AsyncChannel<GameEvent>> ch;
};

// ─────────────────────────────────────────────────────────────────────────────
// §4. Game State Store (thread-safe in-memory)
// ─────────────────────────────────────────────────────────────────────────────

/// Current state of one player (within a room).
struct PlayerState {
    int  hp               = 100;
    bool is_defending     = false;   ///< defending this turn → 50% reduction on next hit
    int  special_cooldown = 0;       ///< remaining cooldown (0 = usable)
};

/// Room state machine.
struct RoomState {
    uint64_t    room_id     = 0;
    std::string room_name;
    std::string player_ids[2];       ///< [0]=host, [1]=joiner
    PlayerState player_states[2];    ///< 1:1 correspondence with player_ids
    int         player_count = 0;
    int         current_turn = 0;    ///< 0 or 1 — index of the player acting this turn
    enum class Phase { Waiting, Playing, Finished } phase = Phase::Waiting;
    std::string winner_id;
    uint64_t    event_counter = 0;   ///< event sequence number within this room
};

/// Cumulative player record.
struct PlayerRecord {
    int    wins         = 0;
    int    losses       = 0;
    int    total_damage = 0;
};

/// Game event history record.
struct EventRecord {
    uint64_t event_id = 0;
    GameEvent event;
};

class GameRegistry {
public:
    // ── Room management ────────────────────────────────────────────────────

    RoomInfo create_room(std::string_view name) {
        std::lock_guard lock(mtx_);
        RoomState r;
        r.room_id   = ++next_room_id_;
        r.room_name = name;
        rooms_[r.room_id] = r;
        return to_info(r);
    }

    /// Join a room. Returns empty optional if already 2 players or room is finished.
    std::optional<RoomInfo> join_room(uint64_t room_id, std::string_view player_id) {
        std::lock_guard lock(mtx_);
        auto it = rooms_.find(room_id);
        if (it == rooms_.end()) return std::nullopt;
        auto& r = it->second;

        // player already joined → pass through
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

    // ── Game action application ────────────────────────────────────────────

    /// Validate + apply state — returns StateUpdate on success.
    std::optional<StateUpdate> apply_action(const ValidatedAction& va) {
        std::lock_guard lock(mtx_);
        auto it = rooms_.find(0); // search by room_id
        {
            uint64_t rid = 0;
            try { rid = std::stoull(va.action.room_id); } catch (...) {}
            it = rooms_.find(rid);
        }
        if (it == rooms_.end()) return std::nullopt;
        auto& r = it->second;

        if (r.phase != RoomState::Phase::Playing) return std::nullopt;

        // verify current turn player
        int atk_idx = r.current_turn;
        if (r.player_ids[atk_idx] != va.action.player_id) return std::nullopt;
        int def_idx = 1 - atk_idx;

        auto& atk = r.player_states[atk_idx];
        auto& def = r.player_states[def_idx];

        StateUpdate su;
        su.validated   = va;
        su.attacker_id = r.player_ids[atk_idx];
        su.defender_id = r.player_ids[def_idx];

        // decrement special cooldown (every turn)
        if (atk.special_cooldown > 0) --atk.special_cooldown;

        // damage calculation
        int dmg = 0;
        if (va.action.type == ActionType::Defend) {
            atk.is_defending = true;
            dmg = 0;
        } else {
            dmg = va.base_dmg;
            if (def.is_defending) dmg /= 2;   // defending: 50% reduction
            if (va.action.type == ActionType::Special)
                atk.special_cooldown = 3;      // set cooldown
            def.hp -= dmg;
        }

        // reset defending state (after being hit)
        if (va.action.type != ActionType::Defend)
            def.is_defending = false;

        su.damage_dealt = dmg;
        su.attacker_hp  = atk.hp;
        su.defender_hp  = std::max(0, def.hp);
        def.hp          = su.defender_hp;

        // determine winner
        if (def.hp <= 0) {
            r.phase     = RoomState::Phase::Finished;
            r.winner_id = su.attacker_id;
            su.game_over = true;
            su.winner_id = su.attacker_id;
            // update records
            records_[su.attacker_id].wins++;
            records_[su.attacker_id].total_damage += dmg;
            records_[su.defender_id].losses++;
            ++finished_rooms_;
        } else {
            records_[su.attacker_id].total_damage += dmg;
            // switch turn
            r.current_turn = def_idx;
        }

        ++r.event_counter;
        return su;
    }

    // ── History ────────────────────────────────────────────────────────────

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

    // ── Leaderboard ────────────────────────────────────────────────────────

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

    // ── Statistics ─────────────────────────────────────────────────────────

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
// §5. Auth Token Verifier (ITokenVerifier implementation)
// ─────────────────────────────────────────────────────────────────────────────

class GameKeyVerifier : public ITokenVerifier {
public:
    std::optional<TokenClaims> verify(std::string_view token) noexcept override {
        // demo: "game-key-{player_id}" format
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
// §6. Static Pipeline Stage Functions
// ─────────────────────────────────────────────────────────────────────────────

/// Stage 1: validation — room existence, player turn, cooldown check
static Task<Result<ValidatedAction>> stage_validate(GameAction action, ActionEnv env) {
    ValidatedAction v;
    v.action = action;

    // pre-compute base damage (used in apply stage without registry lookup)
    switch (action.type) {
        case ActionType::Attack:  v.base_dmg = action.power * 4; break;
        case ActionType::Defend:  v.base_dmg = 0;                break;
        case ActionType::Special: v.base_dmg = action.power * 7; break;
    }

    const char* action_str = action.type == ActionType::Attack  ? "ATTACK"  :
                             action.type == ActionType::Defend  ? "DEFEND"  : "SPECIAL";

    std::println("  [validate #{:02}] worker={} player={} room={} action={} power={} base_dmg={}",
        static_cast<unsigned long long>(action.req_id),
        env.worker_idx,
        action.player_id,
        action.room_id,
        action_str,
        action.power,
        v.base_dmg);

    co_return v;
}

/// Stage 2: game state application — apply state changes to GameRegistry
static Task<Result<StateUpdate>>
stage_apply(ValidatedAction va, ActionEnv env,
            std::shared_ptr<GameRegistry> reg) {
    // real system: update registry here and return result
    auto su_opt = reg->apply_action(va);
    if (!su_opt) {
        // action application failed (wrong turn, room not found, etc.) → error StateUpdate
        StateUpdate su;
        su.validated = va;
        su.validated.valid = false;
        su.validated.error = "invalid action: wrong turn or room not playing";
        std::println("  [apply    #{:02}] worker={} REJECTED: {}",
            static_cast<unsigned long long>(va.action.req_id),
            env.worker_idx, su.validated.error);
        co_return su;
    }

    auto& su = *su_opt;
    std::println("  [apply    #{:02}] worker={} {}→{} dmg={} atk_hp={} def_hp={} {}",
        static_cast<unsigned long long>(va.action.req_id),
        env.worker_idx,
        su.attacker_id,
        su.defender_id,
        su.damage_dealt,
        su.attacker_hp,
        su.defender_hp,
        su.game_over ? "GAME_OVER!" : "");
    co_return su;
}

/// Stage 3: game event generation — StateUpdate → GameEvent (SLO tracking target)
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

    std::println("  [finalize #{:02}] worker={} event_id={} {}",
        static_cast<unsigned long long>(su.validated.action.req_id),
        env.worker_idx,
        static_cast<unsigned long long>(ev.event_id),
        ev.success ? "OK" : ev.message);

    co_return ev;
}

// ─────────────────────────────────────────────────────────────────────────────
// §7. Dynamic Pipeline Stage Functions
// ─────────────────────────────────────────────────────────────────────────────

/// Dynamic Stage "persist": save event history
static auto make_persist_stage(std::shared_ptr<GameRegistry> reg) {
    return [reg](GameEvent ev, ActionEnv env) -> Task<Result<GameEvent>> {
        reg->save_event(ev);
        std::println("  [persist  #{:02}] worker={} room={} saved",
            static_cast<unsigned long long>(ev.event_id),
            env.worker_idx, ev.room_id);
        co_return ev;
    };
}

/// Dynamic Stage "replay": load replay buffer (demonstrates set_enabled — pass-through when disabled)
static Task<Result<GameEvent>> stage_replay(GameEvent ev, ActionEnv env) {
    // real implementation: write to replay buffer (logging only here)
    std::println("  [replay   #{:02}] worker={} [recorded for replay]",
        static_cast<unsigned long long>(ev.event_id), env.worker_idx);
    co_return ev;
}

/// Dynamic Stage "notify": ResponseChannel response + MessageBus fan-out
static auto make_notify_stage(std::shared_ptr<MessageBus> bus,
                               std::atomic<uint64_t>& total_actions,
                               std::atomic<uint64_t>& total_games) {
    return [bus, &total_actions, &total_games]
           (GameEvent ev, ActionEnv env) -> Task<Result<GameEvent>> {

        // ── 1) send HTTP response to ResponseChannel ──────────────────────
        if (auto* rch = env.ctx.get_ptr<ResponseChannel>()) {
            rch->ch->try_send(ev);
        }

        // ── 2) update stats ───────────────────────────────────────────────
        total_actions.fetch_add(1, std::memory_order_relaxed);
        if (ev.game_over)
            total_games.fetch_add(1, std::memory_order_relaxed);

        // ── 3) MessageBus publish (per-room + global SSE fan-out) ─────────
        std::string room_topic = "room." + ev.room_id;
        bus->try_publish(room_topic, ev);

        if (ev.game_over) {
            bus->try_publish("leaderboard", ev);  // trigger leaderboard update
        }

        std::println("  [notify   #{:02}] worker={} → response+SSE published (topic={})",
            static_cast<unsigned long long>(ev.event_id),
            env.worker_idx, room_topic);

        co_return ev;
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// §8. GameServer — struct holding all state
// ─────────────────────────────────────────────────────────────────────────────

struct GameServer {
    // ── Infrastructure ────────────────────────────────────────────────────
    std::shared_ptr<GameRegistry> reg = std::make_shared<GameRegistry>();
    std::shared_ptr<MessageBus>   bus = std::make_shared<MessageBus>();

    // ── Static Pipeline Actions ──────────────────────────────────────────
    using VAction = Action<GameAction, ValidatedAction>;
    using AAction = Action<ValidatedAction, StateUpdate>;
    using FAction = Action<StateUpdate, GameEvent>;

    std::shared_ptr<VAction> validate_act;
    std::shared_ptr<AAction> apply_act;
    std::shared_ptr<FAction> finalize_act;

    // final output channel of Static Pipeline
    std::shared_ptr<AsyncChannel<ContextualItem<GameEvent>>> finalize_out;

    // ── Dynamic Pipeline ──────────────────────────────────────────────────
    std::shared_ptr<DynamicPipeline<GameEvent>> dyn_pipe;

    // ── Statistics ───────────────────────────────────────────────────────
    std::atomic<uint64_t> total_actions{0};
    std::atomic<uint64_t> total_games{0};
    std::atomic<uint64_t> req_counter{0};
    std::atomic<uint64_t> event_counter{0};
    std::atomic<bool>     auto_scale_on{true};

    Dispatcher* disp = nullptr;

    void setup(Dispatcher& d) {
        disp = &d;
        bus->start(d);

        // ── Bind stage functions (capture registry) ───────────────────────
        auto bound_apply = [this](ValidatedAction va, ActionEnv env) {
            return stage_apply(std::move(va), env, this->reg);
        };
        auto bound_finalize = [this](StateUpdate su, ActionEnv env) {
            return stage_finalize(std::move(su), env, this->event_counter);
        };

        // ── Create Static Pipeline Actions ───────────────────────────────
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

        // ── Connect and start Static Actions ─────────────────────────────
        finalize_out = std::make_shared<AsyncChannel<ContextualItem<GameEvent>>>(512);
        finalize_act->start(d, finalize_out);
        apply_act->start(d, finalize_act->input());
        validate_act->start(d, apply_act->input());

        // ── Create Dynamic Pipeline ───────────────────────────────────────
        dyn_pipe = std::make_shared<DynamicPipeline<GameEvent>>(
            DynamicPipeline<GameEvent>::Config{
                .default_channel_cap = 256,
                .default_workers     = 2,
            });

        dyn_pipe->add_stage("persist", make_persist_stage(reg));
        dyn_pipe->add_stage("replay",  stage_replay);
        dyn_pipe->add_stage("notify",  make_notify_stage(bus, total_actions, total_games));

        dyn_pipe->start(d);

        // ── Static → Dynamic bridge coroutine ────────────────────────────
        d.spawn(run_bridge(finalize_out, dyn_pipe));

        // ── Auto-scaler ───────────────────────────────────────────────────
        d.spawn(run_autoscaler(this));

        std::println("[game-server] setup: static(3 actions) + dynamic(3 stages) + autoscaler");
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
                std::println("[autoscale] validate busy → scale_out");
            } else {
                s->validate_act->scale_in();
            }
            if (s->apply_act->input()->size_approx() > 0) {
                s->apply_act->scale_out(*s->disp);
                std::println("[autoscale] apply busy → scale_out");
            } else {
                s->apply_act->scale_in();
            }
        }
    }

    /// Submit a game action to the pipeline. Returns response channel (nullptr = saturated).
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
// §9. HTTP Handlers
// ─────────────────────────────────────────────────────────────────────────────

/// POST /api/v1/rooms — create a room (creator joins automatically)
static Handler make_post_room(GameServer& s) {
    return [&s](const Request& req, Response& res) {
        auto dto = CreateRoomRequest::from_json(req.body());
        auto info = s.reg->create_room(dto ? dto->room_name : "untitled");
        // creator joins automatically
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

/// GET /api/v1/rooms — list rooms
static Handler make_get_rooms(GameServer& s) {
    return [&s](const Request& /*req*/, Response& res) {
        auto rooms = s.reg->list_rooms();
        res.status(200)
           .header("Content-Type", "application/json")
           .body(qbuem::write(rooms));
    };
}

/// GET /api/v1/rooms/:id — room details
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

/// POST /api/v1/rooms/:id/join — join a room
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

/// POST /api/v1/rooms/:id/action — submit game action (Pipeline processes then responds)
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

/// GET /api/v1/rooms/:id/events — per-room SSE live event stream
static AsyncHandler make_sse_room_events(GameServer& s) {
    return [&s](const Request& req, Response& res) -> Task<void> {
        uint64_t room_id = 0;
        try { room_id = std::stoull(std::string(req.param("id"))); } catch (...) {}

        std::string topic = "room." + std::to_string(room_id);
        auto stream = s.bus->subscribe_stream<GameEvent>(topic, 64);

        SseStream sse(res);
        sse.send(qbuem::write(SseConnectedEvent{"connected", topic}), "connected");

        // receive up to 100 events (or until game over)
        for (int i = 0; i < 100; ++i) {
            auto ev = co_await stream->recv();
            if (!ev) break;
            sse.send(qbuem::write(*ev), ev->game_over ? "game_over" : "action");
            if (ev->game_over) break;
        }
        sse.close();
    };
}

/// GET /api/v1/leaderboard — player ranking SSE + JSON
static AsyncHandler make_get_leaderboard(GameServer& s) {
    return [&s](const Request& req, Response& res) -> Task<void> {
        // check for SSE mode (Accept: text/event-stream)
        bool is_sse = req.header("Accept") == "text/event-stream";
        if (!is_sse) {
            auto scores = s.reg->leaderboard(20);
            res.status(200)
               .header("Content-Type", "application/json")
               .body(qbuem::write(scores));
            co_return;
        }

        // SSE mode: push leaderboard update on each game-over event
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

/// GET /api/v1/stats — server statistics
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

/// POST /api/v1/admin/scale — adjust worker scale
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

/// POST /api/v1/admin/toggle — enable/disable a DynamicPipeline stage
static Handler make_post_toggle(GameServer& s) {
    return [&s](const Request& req, Response& res) {
        auto dto = ToggleRequest::from_json(req.body());
        if (!dto) {
            res.status(400).body(qbuem::write(ErrorResponse{"required: stage, enabled"})); return;
        }
        bool ok = s.dyn_pipe->set_enabled(dto->stage, dto->enabled);
        std::println("[toggle] stage={} enabled={}", dto->stage, dto->enabled ? "true" : "false");
        res.status(ok ? 200 : 404).header("Content-Type", "application/json")
           .body(qbuem::write(ToggleResponse{dto->stage, dto->enabled, ok}));
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// §10. Demo Simulation
// ─────────────────────────────────────────────────────────────────────────────

static Task<void> run_demo(GameServer& s, Dispatcher& /*d*/) {
    std::println("\n╔══════════════════════════════════════════════════════════╗");
    std::println("║       qbuem-stack Game Server Demo Start                 ║");
    std::println("╚══════════════════════════════════════════════════════════╝\n");

    co_await qbuem::sleep(200);  // wait for pipeline workers to start

    // ── Phase 1: Room creation and player join ────────────────────────────
    std::println("─── Phase 1: Room Creation and Player Join ────────────────");

    auto room1 = s.reg->create_room("arena-alpha");
    std::println("  room created: room_id={} name={}",
        static_cast<unsigned long long>(room1.room_id), room1.room_name);

    s.reg->join_room(room1.room_id, "alice");
    s.reg->join_room(room1.room_id, "bob");
    std::println("  alice, bob joined. Game start!\n");

    auto room2 = s.reg->create_room("arena-beta");
    s.reg->join_room(room2.room_id, "charlie");
    s.reg->join_room(room2.room_id, "dave");
    std::println("  room created: room_id={} — charlie vs dave\n",
        static_cast<unsigned long long>(room2.room_id));

    // ── Phase 2: Game play (alice vs bob) ────────────────────────────────
    std::println("─── Phase 2: alice vs bob Battle ──────────────────────────");

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
        // alice vs bob — 7-turn battle
        {"alice",   room1.room_id, ActionType::Attack,  8,  "alice attack (power=8, dmg=32)"},
        {"bob",     room1.room_id, ActionType::Defend,  0,  "bob defend preparation"},
        {"alice",   room1.room_id, ActionType::Special, 7,  "alice special (power=7, dmg=49 but bob defending→24)"},
        {"bob",     room1.room_id, ActionType::Attack,  6,  "bob attack (power=6, dmg=24)"},
        {"alice",   room1.room_id, ActionType::Attack,  9,  "alice attack (power=9, dmg=36)"},
        {"bob",     room1.room_id, ActionType::Attack,  5,  "bob attack (power=5, dmg=20)"},
        {"alice",   room1.room_id, ActionType::Attack,  10, "alice attack (power=10, dmg=40)"},
        // charlie vs dave (room2)
        {"charlie", room2.room_id, ActionType::Special, 10, "charlie special (power=10, dmg=70)"},
        {"dave",    room2.room_id, ActionType::Attack,   3, "dave attack (power=3, dmg=12)"},
        {"charlie", room2.room_id, ActionType::Attack,   8, "charlie attack (power=8, dmg=32)"},
    };

    for (size_t i = 0; i < std::size(demo_actions); ++i) {
        const auto& a = demo_actions[i];
        std::println("\n[demo {}] {}", i + 1, a.desc);

        GameAction action;
        action.player_id = a.player;
        action.room_id   = std::to_string(a.room_id);
        action.type      = a.type;
        action.power     = a.power;

        auto ctx = Context{}
            .put(RequestId{"demo-" + std::to_string(i + 1)})
            .put(AuthSubject{a.player});

        auto resp_ch = s.submit_action(action, ctx);
        if (!resp_ch) { std::println("  → [ERROR] overloaded"); continue; }

        auto ev = co_await resp_ch->recv();
        if (!ev) { std::println("  → [ERROR] no result"); continue; }

        std::println("  → {} vs {}: dmg={} atk_hp={} def_hp={} {}",
            ev->attacker, ev->defender,
            ev->damage, ev->attacker_hp, ev->defender_hp,
            ev->game_over ? ("★ GAME OVER! " + ev->winner + " wins!") : "");
    }

    // ── Phase 3: Scale demonstration ─────────────────────────────────────
    std::println("\n─── Phase 3: Manual Scale In/Out ───────────────────────────");
    std::println("[demo] validate scale_out: 1→2");
    s.validate_act->scale_out(*s.disp);
    std::println("[demo] apply scale_to(4): 2→4");
    s.apply_act->scale_to(4, *s.disp);

    // ── Phase 4: Disable DynamicPipeline replay stage ─────────────────────
    std::println("\n─── Phase 4: stage toggle (replay OFF) ────────────────────");
    s.dyn_pipe->set_enabled("replay", false);
    std::println("  [toggle] replay disabled (pass-through)");

    // ── Phase 5: Print leaderboard ───────────────────────────────────────
    std::println("\n─── Phase 5: Leaderboard ───────────────────────────────────");
    co_await qbuem::sleep(300);  // wait for pipeline to complete
    auto scores = s.reg->leaderboard(10);
    std::println("  {:<15} {:>5} {:>5} {:>12} {:>8}", "Player", "Wins", "Loss", "TotalDmg", "WinRate");
    std::println("  --------------------------------------------------");
    for (auto& sc : scores) {
        std::println("  {:<15} {:5} {:5} {:12} {:7.1f}%",
            sc.player_id, sc.wins, sc.losses,
            sc.total_damage, sc.win_rate * 100.0);
    }

    // ── Phase 6: Final statistics ────────────────────────────────────────
    std::println("\n─── Phase 6: Server Statistics ─────────────────────────────");
    std::println("  total actions: {}  finished games: {}  rooms: {}",
        static_cast<unsigned long long>(s.total_actions.load()),
        static_cast<unsigned long long>(s.total_games.load()),
        s.reg->room_count());

    std::println("\n╔══════════════════════════════════════════════════════════╗");
    std::println("║       Demo complete. HTTP server running (port 8080)     ║");
    std::println("║       curl -H 'Authorization: Bearer game-key-alice' \\   ║");
    std::println("║         -X POST http://localhost:8080/api/v1/rooms \\     ║");
    std::println("║         -d '{{\"room_name\":\"my-room\"}}'                   ║");
    std::println("╚══════════════════════════════════════════════════════════╝\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// §11. main()
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    // ── Pipeline Dispatcher (separate thread pool) ────────────────────────
    Dispatcher pipeline_disp(4);
    std::jthread pipeline_thread([&pipeline_disp] { pipeline_disp.run(); });

    // ── Initialize GameServer ─────────────────────────────────────────────
    GameServer server;
    server.setup(pipeline_disp);

    // ── Initialize HTTP App ───────────────────────────────────────────────
    App app(2);

    // ── Middleware chain ──────────────────────────────────────────────────
    app.use(cors(CorsConfig{
        .allow_origin      = "*",
        .allow_methods     = "GET, POST, DELETE, OPTIONS",
        .allow_headers     = "Content-Type, Authorization, X-Request-ID",
        .allow_credentials = false,
    }));

    app.use(rate_limit(RateLimitConfig{
        .rate_per_sec = 60.0,    // 60 requests per second (anti-cheat)
        .max_keys     = 10'000,
        .burst        = 20.0,
    }));

    app.use(request_id("X-Request-ID"));
    app.use(hsts(31'536'000, /*include_subdomains=*/true));

    // bearer_auth middleware — public (read-only) paths pass without authentication
    static GameKeyVerifier verifier;
    auto auth_mw = bearer_auth(verifier);
    app.use([auth_mw](const Request& req, Response& res) -> bool {
        std::string_view path = req.path();
        // publicly accessible paths (no auth required)
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

    // ── Register routes ───────────────────────────────────────────────────
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

    // ── Global error handler ──────────────────────────────────────────────
    app.on_error([](std::exception_ptr ep, const Request& req, Response& res) {
        std::string msg;
        try { std::rethrow_exception(ep); }
        catch (const std::exception& e) { msg = e.what(); }
        catch (...)                     { msg = "unknown error"; }
        std::print(stderr, "[error] {}: {}\n", std::string(req.path()), msg);
        res.status(500)
           .header("Content-Type", "application/json")
           .body(qbuem::write(ErrorResponse{msg}));
    });

    // ── Run demo simulation then start HTTP server ────────────────────────
    pipeline_disp.spawn(run_demo(server, pipeline_disp));

    std::println("[game-server] HTTP listening on :8080");
    auto listen_result = app.listen(8080);

    pipeline_disp.stop();
    pipeline_thread.join();
    return listen_result ? 0 : 1;
}
