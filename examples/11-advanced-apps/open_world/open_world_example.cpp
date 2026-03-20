/**
 * @file examples/11-advanced-apps/open_world/open_world_example.cpp
 * @brief Open-world spatial index using TiledBitset + GridBitset.
 *
 * ## What this example demonstrates
 *
 *   1. **TiledBitset<256, 256, 16>** as an infinite world spatial index.
 *      World coordinates are `int64_t` — the map extends to ±2^63 in both
 *      axes.  Only tiles that receive writes are allocated in memory.
 *
 *   2. **Layer layout** (16 layers per cell):
 *      | Layer | Meaning         |
 *      |-------|-----------------|
 *      |   0   | solid wall      |
 *      |   1   | player entity   |
 *      |   2   | friendly NPC    |
 *      |   3   | monster         |
 *      |   4   | item / pickup   |
 *      |  5–7  | game state flags|
 *
 *   3. **Cross-tile operations** — entities placed on both sides of tile
 *      boundaries; box/radius/raycast queries span multiple tiles transparently.
 *
 *   4. **Aggro detection** — `any_in_radius` to test if a monster can "see"
 *      a player within its aggro radius.  Runs in ~8 ns (TLS hit, early exit).
 *
 *   5. **AoE spell** — `count_in_radius` to count entities hit by a fireball.
 *
 *   6. **Line-of-sight** — cross-tile `raycast` (Bresenham DDA) to verify
 *      the player has an unobstructed view to a target.
 *
 *   7. **Minimap / fog of war** — `for_each_set_in_box` in a 64×64 viewport.
 *
 *   8. **Dynamic tile lifecycle** — entity despawn + `evict_empty_tiles()`
 *      to reclaim memory for abandoned world regions.
 *
 * ## Scenario
 *   A player roams an infinite world.  Three dungeon zones are placed at
 *   different world positions (one deliberately across a tile boundary,
 *   one at negative coordinates).  The game loop runs several "ticks":
 *
 *     Tick 1 — World spawn: place walls, NPCs, monsters, items.
 *     Tick 2 — Combat frame: aggro + AoE + LoS checks for every monster.
 *     Tick 3 — Minimap scan: enumerate nearby entities for the HUD.
 *     Tick 4 — Player moves: update entity layer at new position.
 *     Tick 5 — Cleanup: despawn distant entities, evict empty tiles.
 *
 * ## Build
 *   cmake --build build --target open_world_example
 *
 * ## Run
 *   ./build/examples/open_world_example
 */

#include <qbuem/buf/grid_bitset.hpp>
#include <qbuem/buf/tiled_bitset.hpp>
#include <qbuem/compat/print.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string_view>

using namespace qbuem;

// ── World constants ───────────────────────────────────────────────────────────

// Tile: 256 × 256 cells, 16 layers.  One tile ≈ 512 KB.
static constexpr uint32_t kTW = 256;
static constexpr uint32_t kTH = 256;
static constexpr uint32_t kD  = 16;

// Layer indices — fits in 4 bits, all values < kD.
static constexpr uint32_t kLayerWall    = 0;
static constexpr uint32_t kLayerPlayer  = 1;
static constexpr uint32_t kLayerNPC     = 2;
static constexpr uint32_t kLayerMonster = 3;
static constexpr uint32_t kLayerItem    = 4;

using World = TiledBitset<kTW, kTH, kD>;

// ── Timing helper ─────────────────────────────────────────────────────────────

static uint64_t now_ns() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
        .count());
}

// ── Pretty-print helpers ──────────────────────────────────────────────────────

static void section(std::string_view title) {
    std::println("\n╔══════════════════════════════════════════════════════════╗");
    std::print("║  {:<57}║\n", title);
    std::println("╚══════════════════════════════════════════════════════════╝");
}

static void ok(std::string_view msg) {
    std::println("  \033[32m✓\033[0m {}", msg);
}

static void info(std::string_view msg) {
    std::println("  · {}", msg);
}

// ── Entity type name ──────────────────────────────────────────────────────────

static std::string_view entity_name(uint64_t layer_mask) noexcept {
    if (layer_mask & (uint64_t{1} << kLayerWall))    return "Wall";
    if (layer_mask & (uint64_t{1} << kLayerPlayer))  return "Player";
    if (layer_mask & (uint64_t{1} << kLayerNPC))     return "NPC";
    if (layer_mask & (uint64_t{1} << kLayerMonster)) return "Monster";
    if (layer_mask & (uint64_t{1} << kLayerItem))    return "Item";
    return "Unknown";
}

// ── Dungeon zone descriptor ───────────────────────────────────────────────────

struct Zone {
    std::string_view name;
    World::WorldCoord cx, cy;  // zone center in world coordinates
};

// Three zones that span different tile regions:
//   Zone A — entirely inside tile (0, 0).
//   Zone B — straddles the tile boundary at x = 256 (tiles (0,0) and (1,0)).
//   Zone C — at negative world coordinates, in tile (-2, -2).
//             Completely isolated from the outer boundary walls → clean eviction.
static constexpr std::array<Zone, 3> kZones{{
    {"Ashfield Keep",  120,   100},  // tile (0,0)
    {"Bridge of Fate", 250,   120},  // straddles tile boundary (tiles 0 and 1)
    {"Shadowfen Vale", -370, -370},  // tile (-2,-2) — isolated, no shared walls
}};

// ── Spawn helpers ─────────────────────────────────────────────────────────────

// Place a rectangular wall outline around (cx, cy) with half-size (hw, hh).
static void place_wall_rect(World& w,
                             World::WorldCoord cx, World::WorldCoord cy,
                             int32_t hw, int32_t hh) {
    for (int32_t dx = -hw; dx <= hw; ++dx) {
        w.set(cx + dx, cy - hh, kLayerWall);
        w.set(cx + dx, cy + hh, kLayerWall);
    }
    for (int32_t dy = -hh + 1; dy < hh; ++dy) {
        w.set(cx - hw, cy + dy, kLayerWall);
        w.set(cx + hw, cy + dy, kLayerWall);
    }
}

// Spawn N monsters in a ring of radius r around (cx, cy).
static void spawn_monster_ring(World& w,
                                World::WorldCoord cx, World::WorldCoord cy,
                                uint32_t n, int32_t r) {
    for (uint32_t i = 0; i < n; ++i) {
        const double angle = 2.0 * 3.14159265 * i / n;
        const auto mx = cx + static_cast<World::WorldCoord>(r * std::cos(angle));
        const auto my = cy + static_cast<World::WorldCoord>(r * std::sin(angle));
        w.set(mx, my, kLayerMonster);
    }
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main() {
    std::println("╔══════════════════════════════════════════════════════════╗");
    std::println("║  qbuem-stack — Open World Example (TiledBitset)          ║");
    std::println("╚══════════════════════════════════════════════════════════╝");
    std::println("  World grid: TiledBitset<{}, {}, {}>", kTW, kTH, kD);
    std::println("  Tile size : {} KB  ({} × {} × {} layers × 8 bytes)",
                 kTW * kTH * 8 / 1024, kTW, kTH, kD);
    std::println("  Coord range: int64_t  (±2^63 in both axes)");

    World world;

    // ──────────────────────────────────────────────────────────────────────────
    section("Tick 1 — World spawn");

    // Place a large wall boundary at the outer edges of the playable area.
    // Spans 4 tiles horizontally (tiles –1, 0, 1, 2).
    info("Placing outer boundary walls (spans 4 tiles across x-axis)...");
    place_wall_rect(world, 256, 0, 260, 200);

    // Each dungeon zone gets a smaller inner wall + monsters + NPCs + items.
    for (const auto& zone : kZones) {
        std::println("  Spawning zone \033[33m{}\033[0m at ({}, {})",
                     zone.name, zone.cx, zone.cy);

        place_wall_rect(world, zone.cx, zone.cy, 20, 15);
        spawn_monster_ring(world, zone.cx, zone.cy, 4, 12);

        // A friendly NPC and a pickup at the zone center.
        world.set(zone.cx,     zone.cy,     kLayerNPC);
        world.set(zone.cx + 2, zone.cy + 1, kLayerItem);
    }

    // Player starts at origin (tile 0,0).
    const World::WorldCoord player_x = 0, player_y = 0;
    world.set(player_x, player_y, kLayerPlayer);

    std::println("  Loaded tiles : {}", world.loaded_tile_count());
    std::println("  Memory usage : {} KB", world.memory_bytes() / 1024);
    ok("World spawn complete");

    // ──────────────────────────────────────────────────────────────────────────
    section("Tick 2 — Combat: aggro + AoE + line-of-sight");

    // Aggro check: does any monster within radius 15 of the player detect them?
    {
        const uint32_t aggro_r = 15;
        const uint64_t t0 = now_ns();
        const bool monster_nearby = world.any_in_radius(
            player_x, player_y, aggro_r, kLayerMonster, kLayerMonster);
        const uint64_t elapsed = now_ns() - t0;
        std::println("  any_in_radius r={} (monster aggro): {} — {} ns",
                     aggro_r,
                     monster_nearby ? "\033[31mAGGRO TRIGGERED\033[0m"
                                    : "\033[32mno aggro\033[0m",
                     elapsed);
    }

    // AoE fireball: player casts at (120, 100) — hits all entity types.
    {
        const World::WorldCoord fx = kZones[0].cx, fy = kZones[0].cy;
        const uint32_t aoe_r = 15;
        const uint64_t t0 = now_ns();
        const uint32_t hits = world.count_in_radius(
            fx, fy, aoe_r, 0, kD - 1);  // all layers
        const uint64_t elapsed = now_ns() - t0;
        std::println("  count_in_radius r={} at ({},{}) — {} entities hit — {} ns",
                     aoe_r, fx, fy, hits, elapsed);
        ok("AoE query complete");
    }

    // Line-of-sight: does the player at (0,0) have LoS to Shadowfen Vale at (−80,−60)?
    // Walls are at tile boundaries so the ray from (0,0) → (−80,−60) should cross
    // the outer boundary walls at negative coordinates.
    {
        const World::WorldCoord tx = kZones[2].cx, ty = kZones[2].cy;
        const int32_t dx = (tx > player_x) ? 1 : -1;
        const int32_t dy = (ty > player_y) ? 1 : -1;
        const uint64_t t0 = now_ns();
        auto hit = world.raycast(
            player_x, player_y, dx, dy,
            kLayerWall, kLayerWall, 200u);
        const uint64_t elapsed = now_ns() - t0;
        if (hit) {
            std::println("  raycast → wall hit at ({}, {}) after {} steps — {} ns",
                         hit->wx, hit->wy, hit->steps, elapsed);
        } else {
            std::println("  raycast → open path to ({}, {}) — {} ns",
                         tx, ty, elapsed);
        }
    }

    // Cross-tile LoS: player at (0,0) → "Bridge of Fate" zone at (250, 120).
    // The ray must cross tile boundary at x=256.
    {
        const World::WorldCoord tx = kZones[1].cx, ty = kZones[1].cy;
        const uint64_t t0 = now_ns();
        auto hit = world.raycast(player_x, player_y, 1, 0,
                                 kLayerWall, kLayerWall, 280u);
        const uint64_t elapsed = now_ns() - t0;
        if (hit) {
            std::println("  cross-tile raycast (±x=256 boundary) → wall at ({},{}) — {} ns",
                         hit->wx, hit->wy, elapsed);
            if (World::tile_x(hit->wx) != World::tile_x(player_x))
                ok("Ray successfully crossed tile boundary");
        } else {
            std::println("  cross-tile raycast → open at x=+280 — {} ns", elapsed);
        }
    }

    // ──────────────────────────────────────────────────────────────────────────
    section("Tick 3 — Minimap: enumerate nearby entities");

    // Scan a 64×64 viewport centred on zone A for the HUD.
    {
        const World::WorldCoord vx = kZones[0].cx, vy = kZones[0].cy;
        const int32_t half = 32;
        uint32_t total = 0;
        std::array<uint32_t, kD> per_layer{};

        const uint64_t t0 = now_ns();
        world.for_each_set_in_box(
            vx - half, vy - half, vx + half, vy + half,
            0, kD - 1,
            [&](World::WorldCoord, World::WorldCoord, uint64_t mask) noexcept {
                ++total;
                for (uint32_t l = 0; l < kD; ++l)
                    if (mask & (uint64_t{1} << l)) ++per_layer[l];
            });
        const uint64_t elapsed = now_ns() - t0;

        std::println("  for_each_set_in_box 64×64 around ({},{}) — {} entities — {} ns",
                     vx, vy, total, elapsed);
        for (uint32_t l = 0; l < kD; ++l)
            if (per_layer[l])
                std::println("    layer {:2d}: {} cells", l, per_layer[l]);
        ok("Minimap scan complete");
    }

    // Radius scan: list all entities within 8 cells of zone B center (cross-tile).
    {
        const World::WorldCoord rx = kZones[1].cx, ry = kZones[1].cy;
        const uint32_t r = 15;
        std::println("  for_each_set_in_radius r={} around {} ({},{}):",
                     r, kZones[1].name, rx, ry);
        uint32_t cnt = 0;
        world.for_each_set_in_radius(rx, ry, r, 0, kD - 1,
            [&](World::WorldCoord wx, World::WorldCoord wy, uint64_t mask) noexcept {
                if (cnt < 6)
                    std::println("    ({:5},{:5}) {} [tile ({},{})]",
                                 wx, wy, entity_name(mask),
                                 World::tile_x(wx), World::tile_y(wy));
                ++cnt;
            });
        if (cnt > 6)
            std::println("    … and {} more", cnt - 6);
        std::println("  Total found: {}", cnt);
        ok("Radius entity scan complete");
    }

    // ──────────────────────────────────────────────────────────────────────────
    section("Tick 4 — Player moves");

    {
        // Move player from (0,0) to near Ashfield Keep.
        const World::WorldCoord new_x = kZones[0].cx - 25;
        const World::WorldCoord new_y = kZones[0].cy - 10;

        world.clear(player_x, player_y, kLayerPlayer);
        world.set(new_x, new_y, kLayerPlayer);

        std::println("  Player moved: ({},{}) → ({},{})", player_x, player_y, new_x, new_y);
        std::println("  New tile: ({},{})", World::tile_x(new_x), World::tile_y(new_y));

        // Re-check aggro from new position.
        const bool aggro = world.any_in_radius(new_x, new_y, 20,
                                               kLayerMonster, kLayerMonster);
        std::println("  Aggro check from new pos (r=20): {}",
                     aggro ? "\033[31mTRIGGERED\033[0m" : "\033[32mclear\033[0m");
        ok("Player position updated");
    }

    // ──────────────────────────────────────────────────────────────────────────
    section("Tick 5 — Cleanup: despawn distant entities, evict empty tiles");

    {
        std::println("  Tiles before eviction: {}", world.loaded_tile_count());
        std::println("  Memory before: {} KB", world.memory_bytes() / 1024);

        // Despawn all entities in Shadowfen Vale zone (simulate "out of range").
        const auto& v = kZones[2];
        world.for_each_set_in_box(
            v.cx - 25, v.cy - 20, v.cx + 25, v.cy + 20,
            0, kD - 1,
            [&](World::WorldCoord wx, World::WorldCoord wy, uint64_t) noexcept {
                world.clear_all(wx, wy);
            });

        const size_t evicted = world.evict_empty_tiles();
        std::println("  Tiles evicted (now empty): {}", evicted);
        std::println("  Tiles after eviction     : {}", world.loaded_tile_count());
        std::println("  Memory after : {} KB", world.memory_bytes() / 1024);
        ok("Memory reclaimed");
    }

    // ──────────────────────────────────────────────────────────────────────────
    section("World summary — coordinate span");

    // Show that tiles span negative and positive coordinates correctly.
    std::println("  for_each_tile:");
    world.for_each_tile([](World::TileCoord tx, World::TileCoord ty,
                            const World::Tile& t) noexcept {
        const uint32_t cnt = t.count_in_box(0, 0, kTW - 1, kTH - 1, 0, kD - 1);
        std::println("    tile ({:3},{:3})  world [{:6},{:6}] × [{:6},{:6}]  {:4} occupied cells",
                     tx, ty,
                     World::world_x(tx, 0),  World::world_x(tx, kTW - 1),
                     World::world_y(ty, 0),  World::world_y(ty, kTH - 1),
                     cnt);
    });

    std::println("\n  Total loaded tiles : {}", world.loaded_tile_count());
    std::println("  Total memory       : {} KB\n", world.memory_bytes() / 1024);
    ok("Open-world example complete");
    return 0;
}
