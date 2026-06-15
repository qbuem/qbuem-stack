/**
 * @file examples/01-foundation/config/config_example.cpp
 * @brief ConfigManager + Secret<T> — secure, zero-allocation-at-access config.
 *
 * Demonstrates the qbuem::config module:
 *   1. Layered config loading (defaults → env → explicit set)
 *   2. Typed reads via get_or<T>() for int / string / bool / double
 *   3. String→typed coercion (env/file values arrive as strings)
 *   4. get_secret() returning a move-only Secret<T> that:
 *        - masks itself in std::print/std::format ("[REDACTED]")
 *        - requires explicit reveal() at every audited call site
 *        - wipes its backing bytes on destruction (RAII zeroing)
 *   5. The missing-key default path (get_or fallback + get_secret error).
 *
 * Pillars honoured (teaching material):
 *   - Zero Allocation: get_or()/get_secret()/contains() are O(1) lock-free reads
 *     over a fixed flat hash table — no heap traffic on the access path.
 *   - Zero Copy: keys are passed as std::string_view; values returned by value
 *     (trivially-copyable scalars) or as zero-copy string_view.
 *   - C++23: std::expected return types, std::print/std::println, if constexpr
 *     dispatch inside get_or<T>(), std::to_underlying for the enum tag.
 */

#include <qbuem/config/config_manager.hpp>

#include <string>
#include <string_view>
#include <utility> // std::to_underlying

#include <qbuem/compat/print.hpp>

namespace cfg = qbuem::config;
using std::print;
using std::println;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static void banner(std::string_view title) {
    println("");
    println("┌──────────────────────────────────────────────────────────────┐");
    println("│ {:<60} │", title);
    println("└──────────────────────────────────────────────────────────────┘");
}

static std::string_view type_name(cfg::ConfigValue::Type t) {
    switch (t) {
        case cfg::ConfigValue::Type::Int:    return "Int";
        case cfg::ConfigValue::Type::Double: return "Double";
        case cfg::ConfigValue::Type::Bool:   return "Bool";
        case cfg::ConfigValue::Type::String: return "String";
        case cfg::ConfigValue::Type::Unset:  return "Unset";
    }
    return "?";
}

// ─── Section 1 — typed registration & reads ─────────────────────────────────────

static void demo_typed_values(const cfg::ConfigManager& cm) {
    banner("1. Typed reads — get_or<T>() (int / string / bool / double)");

    // O(1), lock-free, zero-heap reads. Default is returned only on miss.
    const auto port    = cm.get_or<std::int64_t>("server.port", 8080);
    const auto host    = cm.get_or<std::string_view>("server.host", "127.0.0.1");
    const auto debug   = cm.get_or<bool>("server.debug", false);
    const auto ratio   = cm.get_or<double>("server.load_ratio", 0.0);
    const auto workers = cm.get_or<std::int64_t>("server.workers", 1);

    println("  server.host       = {}", host);
    println("  server.port       = {}", port);
    println("  server.workers    = {}", workers);
    println("  server.debug      = {}", debug);
    println("  server.load_ratio = {:.2f}", ratio);
}

// ─── Section 2 — layered override (env / set beats default) ──────────────────────

static void demo_layering(const cfg::ConfigManager& cm) {
    banner("2. Layered override — explicit set() / env beats set_default()");

    // server.port was registered as a default (8080) then overridden via set(9090).
    // The highest-priority write wins; the read sees only the final value.
    println("  contains(\"server.port\")     = {}", cm.contains("server.port"));
    println("  server.port (after override) = {}",
            cm.get_or<std::int64_t>("server.port", -1));

    // Env-loaded values arrive as strings and are coerced on demand by get_or<T>.
    // We seeded "server.workers" via set() as a string to mirror that path.
    println("  server.workers (string→int) = {}",
            cm.get_or<std::int64_t>("server.workers", -1));
}

// ─── Section 3 — Secret<T>: masked, audited, self-wiping ─────────────────────────

static void demo_secret(const cfg::ConfigManager& cm) {
    banner("3. get_secret() — masked print, explicit reveal, RAII wipe");

    auto api_key = cm.get_secret("external.api_key");
    if (!api_key) {
        println("  unexpected: api_key missing ({})", api_key.error().message());
        return;
    }

    // A Secret<> ALWAYS masks itself in std::format / std::print.
    // There is no way to accidentally log the raw bytes.
    println("  std::print(secret)  -> {}", *api_key);          // [REDACTED]
    println("  has_value()          -> {}", api_key->has_value());

    // reveal() is the single, intentionally-verbose, auditable access point.
    // We only show a short prefix here to prove it works without dumping it.
    const std::string& raw = api_key->reveal();
    const auto prefix = std::string_view{raw}.substr(0, 4);
    println("  reveal() prefix      -> {}… ({} bytes total)", prefix, raw.size());

    // RAII wipe demonstration: a Secret scoped here zeroes its bytes on exit.
    // We capture the backing pointer/size to inspect the buffer afterwards.
    const char* observed_ptr = nullptr;
    std::size_t observed_len = 0;
    {
        cfg::Secret<std::string> tmp{std::string{"top-secret-token-XYZ"}};
        const std::string& tmp_raw = tmp.reveal();
        observed_ptr = tmp_raw.data();
        observed_len = tmp_raw.size();
        println("  scoped secret alive  -> {} bytes (masked: {})",
                observed_len, tmp);
    } // ~Secret() runs a volatile byte-clear here.

    // NOTE: reading freed/moved storage is technically UB; this is illustrative
    // only. The destructor's volatile clear prevents dead-store elimination so a
    // memory scraper cannot recover the value after scope exit.
    bool all_zero = true;
    if (observed_ptr != nullptr) {
        for (std::size_t i = 0; i < observed_len; ++i) {
            if (observed_ptr[i] != '\0') { all_zero = false; break; }
        }
    }
    println("  after scope: backing buffer zeroed = {} (RAII wipe)", all_zero);
}

// ─── Section 4 — missing-key paths ──────────────────────────────────────────────

static void demo_missing_keys(const cfg::ConfigManager& cm) {
    banner("4. Missing-key paths — get_or fallback + get_secret error");

    // get_or returns the supplied default on a miss (never throws, never allocs).
    const auto timeout = cm.get_or<std::int64_t>("server.timeout_ms", 30'000);
    println("  get_or(\"server.timeout_ms\", 30000) -> {} (default, key absent)",
            timeout);
    println("  contains(\"server.timeout_ms\")        -> {}",
            cm.contains("server.timeout_ms"));

    // get_secret returns std::expected — a missing key is a value-error, not UB.
    auto missing = cm.get_secret("external.missing_key");
    if (!missing) {
        println("  get_secret(\"external.missing_key\")   -> error: \"{}\"",
                missing.error().message());
    } else {
        println("  unexpected: missing key resolved");
    }

    // get_secret on a non-string value is rejected with invalid_argument:
    // server.port is stored as Int, so it cannot be revealed as a secret string.
    auto wrong_type = cm.get_secret("server.port");
    if (!wrong_type) {
        println("  get_secret(\"server.port\") [Int]       -> error: \"{}\"",
                wrong_type.error().message());
    }
}

// ─── main ───────────────────────────────────────────────────────────────────────

int main() {
    println("");
    println("════════════════════════════════════════════════════════════════");
    println("  qbuem::config — ConfigManager + Secret<T> demonstration");
    println("  Zero allocation at access · masked secrets · std::expected");
    println("════════════════════════════════════════════════════════════════");

    cfg::ConfigManager cm;

    // ── Init phase (NOT thread-safe — call before serving requests) ──
    // set_default(): lowest priority, will NOT overwrite an existing entry.
    cm.set_default("server.host",       cfg::ConfigValue{std::string_view{"0.0.0.0"}});
    cm.set_default("server.port",       cfg::ConfigValue{std::int64_t{8080}});
    cm.set_default("server.workers",    cfg::ConfigValue{std::int64_t{4}});
    cm.set_default("server.debug",      cfg::ConfigValue{false});
    cm.set_default("server.load_ratio", cfg::ConfigValue{0.75});

    // set(): highest priority — always overrides. Mimics a runtime/env override.
    cm.set("server.port", cfg::ConfigValue{std::int64_t{9090}});

    // Env/file values arrive as STRINGS; get_or<int64_t>() coerces on demand.
    // Seed one as a string to exercise the string→int coercion branch.
    cm.set("server.workers", cfg::ConfigValue{std::string_view{"8"}});

    // A sensitive value — only ever exposed through get_secret() / reveal().
    cm.set("external.api_key",
           cfg::ConfigValue{std::string_view{"sk_live_4f2a9d7e_demo_key"}});

    // Quick peek at the tagged-union type system (std::to_underlying = C++23).
    {
        const cfg::ConfigValue v{std::int64_t{42}};
        println("");
        println("  ConfigValue tag demo: type={} (enum #{}) is_set={}",
                type_name(v.type()),
                std::to_underlying(v.type()),
                v.is_set());
    }

    demo_typed_values(cm);
    demo_layering(cm);
    demo_secret(cm);
    demo_missing_keys(cm);

    banner("Done");
    println("  All reads are O(1), lock-free, and heap-free after init.");
    println("");
    return 0;
}
