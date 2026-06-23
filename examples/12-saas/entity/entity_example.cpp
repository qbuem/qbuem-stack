/**
 * @file examples/12-saas/entity/entity_example.cpp
 * @brief Reusable, extensible Entity framework — one pattern for game AND web
 *        entities, built on qbuem-json (`qbuem/entity/entity_router.hpp`).
 *
 * The idea: an "entity" is any plain struct + `QBUEM_JSON_FIELDS` (so it already
 * has JSON + CBOR). `EntityRouter` adds the cross-project reuse layer — register
 * your own types by name, encode them into a self-describing envelope, and
 * dispatch a heterogeneous stream by type tag. No base class, no vtable; each
 * project (game, web server) registers its own entities and extends freely.
 *
 * This runs standalone (no socket): the envelope is just bytes — in production
 * you'd put it on a TcpStream, an SHMChannel (as a byte span), a save file, or a
 * DB blob, and `dispatch()` on the far side.
 */

#include <qbuem/entity/entity_router.hpp>

#include <print>
#include <string>
#include <vector>

// ── Game entities (extend with your own — no central registry edit needed) ────
enum class Faction { Neutral, Hostile, Boss };
struct Stats     { int hp; int atk; int def; };
struct Weapon    { std::string name; int damage; float range; bool two_handed; };
struct Monster   { std::string id; Faction faction; Stats stats; std::vector<std::string> drops; };
struct Character { std::string name; int level; Stats stats; std::vector<std::string> inventory; };
QBUEM_JSON_FIELDS(Stats,     hp, atk, def)
QBUEM_JSON_FIELDS(Weapon,    name, damage, range, two_handed)
QBUEM_JSON_FIELDS(Monster,   id, faction, stats, drops)
QBUEM_JSON_FIELDS(Character, name, level, stats, inventory)

// ── Web / SaaS entities — same mechanism, different domain ────────────────────
struct User  { std::string id; std::string email; bool active; };
struct Order { std::string id; std::string user_id; double total; std::vector<std::string> items; };
QBUEM_JSON_FIELDS(User,  id, email, active)
QBUEM_JSON_FIELDS(Order, id, user_id, total, items)

namespace {
int passed = 0, failed = 0;
void check(std::string_view what, bool ok) {
  std::println("  [{}] {}", ok ? "PASS" : "FAIL", what);
  ok ? ++passed : ++failed;
}
} // namespace

int main() {
  using namespace qbuem::entity;
  std::println("=== Extensible Entity framework (game + web, on qbuem-json) ===\n");

  EntityRouter r;
  int weapons = 0, monsters = 0, chars = 0, users = 0, orders = 0;
  std::string last;

  // Each project registers its own entity types + handlers (the extension point).
  r.on<Weapon>("weapon",       [&](const Weapon& w)    { ++weapons;  last = w.name; });
  r.on<Monster>("monster",     [&](const Monster& m)   { ++monsters; last = m.id; });
  r.on<Character>("character", [&](const Character& c) { ++chars;    last = c.name; });
  r.on<User>("user",           [&](const User& u)      { ++users;    last = u.email; });
  r.on<Order>("order",         [&](const Order& o)     { ++orders;   last = o.id; });
  std::println("registered {} entity types\n", r.registered_count());

  // ── 1. JSON I/O for a single typed entity (config / SaaS API / save) ─────────
  std::string wj = qbuem::write(Weapon{"Excalibur", 120, 2.5f, true});
  std::println("JSON: {}", wj);
  Weapon wb = qbuem::read<Weapon>(wj);
  check("JSON round-trip", wb.name == "Excalibur" && wb.two_handed);

  // ── 2. A heterogeneous entity stream (game replication / web event bus) ──────
  //     encode() → self-describing bytes; dispatch() → typed handler by tag.
  std::vector<std::string> wire = {
      r.encode(Weapon{"Bow", 40, 12.0f, false}),
      r.encode(Monster{"goblin", Faction::Hostile, {30, 7, 2}, {"gold", "dagger"}}),
      r.encode(Character{"Aria", 12, {220, 35, 18}, {"potion", "map"}}),
      r.encode(User{"u_1", "a@example.com", true}),
      r.encode(Order{"o_9", "u_1", 49.95, {"sku-1", "sku-2"}}),
  };
  for (const auto& bytes : wire) (void)r.dispatch(bytes);

  check("weapon dispatched",    weapons == 1);
  check("monster dispatched",   monsters == 1);
  check("character dispatched", chars == 1);
  check("user dispatched",      users == 1);
  check("order dispatched",     orders == 1);
  std::println("  (envelope sizes: weapon={}B, order={}B — these go on a socket/SHM as-is)",
               wire[0].size(), wire[4].size());

  // ── 3. Boundary safety: unknown type / malformed bytes never throw ───────────
  std::string unknown; unknown.push_back(0); unknown.push_back(7); unknown += "unknown";
  check("unknown type rejected",  !r.dispatch(unknown));
  check("malformed bytes rejected", !r.dispatch(std::string(1, '\xFF')));

  std::println("\n{} passed, {} failed", passed, failed);
  std::println("\nReuse: a game registers Weapon/Monster/...; a web server registers");
  std::println("User/Order/...; both get tagged serialize + dispatch over any transport.");
  return failed == 0 ? 0 : 1;
}
