# game_server

**Category:** Advanced Applications
**File:** `game_server.cpp`
**Complexity:** Expert
**Dependencies:** `qbuem-json`

## Overview

A real-time multiplayer turn-based battle game server. Combines REST API (game management), `StaticPipeline` (action validation/application/finalization), `DynamicPipeline` (persist/replay/notify), `MessageBus` (per-room SSE + leaderboard SSE), `GameRegistry` (in-memory game state), JWT Bearer auth, and rate limiting.

## Scenario

Players connect to a battle game server, create/join game rooms, and submit actions (attack, defend, use item). Each action is processed through a validated pipeline. Room state updates are broadcast via SSE to all players in the room. A global leaderboard is updated and broadcast separately.

## Architecture Diagram

```
  ┌──────────────────────────────────────────────────────────────┐
  │  qbuem-stack Game Server                                    │
  │                                                              │
  │  REST API                                                    │
  │  ┌────────────────────────────────────────────────────────┐  │
  │  │  POST /api/games          → create game room           │  │
  │  │  POST /api/games/:id/join → join room                  │  │
  │  │  POST /api/games/:id/action → submit game action       │  │
  │  │  GET  /api/games/:id      → get game state             │  │
  │  │  GET  /rooms/:id/events   → SSE room updates           │  │
  │  │  GET  /leaderboard/events → SSE leaderboard stream     │  │
  │  └───────────────────────────┬────────────────────────────┘  │
  │                              │  (Bearer JWT auth required)    │
  │                              │  (Rate limit: 10 actions/s)    │
  │                              │                                │
  │  GameAction (JSON) ──────────▼                               │
  │  ┌────────────────────────────────────────────────────────┐  │
  │  │  StaticPipeline                                        │  │
  │  │  [validate]─►[apply_action]─►[finalize]               │  │
  │  │   check turn  calc damage     update HP, status        │  │
  │  └───────────────────────────┬────────────────────────────┘  │
  │                              │                                │
  │  ┌────────────────────────────────────────────────────────┐  │
  │  │  DynamicPipeline                                       │  │
  │  │  [persist]─►[replay_check]─►[notify]                  │  │
  │  │  save state   check for replay  trigger SSE            │  │
  │  └───────────────────────────┬────────────────────────────┘  │
  │                              │                                │
  │        ┌─────────────────────┤                               │
  │        │                     │                               │
  │        ▼                     ▼                               │
  │  GameRegistry         MessageBus                             │
  │  (in-memory state)    ├─ "room.<id>"  → room SSE clients    │
  │                       └─ "leaderboard" → leaderboard SSE     │
  └──────────────────────────────────────────────────────────────┘
```

## Key Features Demonstrated

| Feature | Implementation |
|---------|---------------|
| Turn-based game logic | `StaticPipeline` validate → apply → finalize |
| Persistent state | `DynamicPipeline` persist → replay_check → notify |
| Real-time push | `MessageBus` → SSE per room + global leaderboard |
| Authentication | JWT Bearer token (`JwtAuthAction`) |
| Anti-cheat | Rate limiting (10 actions/sec per player) |
| JSON deserialization | `qbuem_json` for `GameAction` parsing |

## API

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| `POST` | `/api/auth/token` | None | Get JWT token |
| `POST` | `/api/games` | JWT | Create game room |
| `POST` | `/api/games/:id/join` | JWT | Join room |
| `POST` | `/api/games/:id/action` | JWT + rate-limit | Submit action |
| `GET` | `/api/games/:id` | JWT | Get game state |
| `GET` | `/rooms/:id/events` | JWT | SSE room stream |
| `GET` | `/leaderboard/events` | JWT | SSE leaderboard |

## Input

```json
POST /api/games/{id}/action
Authorization: Bearer <jwt>
{
  "action_type": "ATTACK",
  "target_player": "player_2",
  "skill_id": 42
}
```

## Output

```json
{
  "result": "HIT",
  "damage": 85,
  "target_hp_remaining": 215,
  "game_state": "IN_PROGRESS",
  "next_turn": "player_2"
}
```

SSE events:
```
event: game_update
data: {"room":"room-001","hp":{"p1":300,"p2":215},"turn":"player_2"}
```

## How to Run

```bash
# Requires qbuem-json
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target game_server
./build/examples/11-advanced-apps/game_server/game_server
# Server starts on :8091
```

## Notes

- JWTs are validated using qbuem-stack's SIMD JWT parser — no OpenSSL dependency.
- Rate limiting uses a per-player token bucket (10 req/s) to prevent action spamming.
- The leaderboard SSE channel receives updates whenever any game ends.
