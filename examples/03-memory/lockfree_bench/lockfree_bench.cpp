/**
 * @file examples/03-memory/lockfree_bench/lockfree_bench.cpp
 * @brief Lock-free primitive benchmarks vs. standard library equivalents.
 *
 * ## What this example demonstrates
 *
 * Direct performance comparison of qbuem-stack's zero-allocation primitives
 * against their standard-library counterparts:
 *
 *   1. `LockFreeHashMap<uint64_t, uint32_t>` vs. `std::mutex + std::unordered_map`
 *   2. `IntrusiveList<T>` vs. `std::list<T>` (heap-allocating)
 *   3. `GenerationPool<T>` vs. `std::vector<std::shared_ptr<T>>`
 *
 * Each benchmark runs both variants for the same workload and prints
 * throughput and latency statistics.
 *
 * ## Build
 *   cmake --build build --target lockfree_bench
 *
 * ## Run
 *   ./build/examples/lockfree_bench
 *
 * ## Expected output (approximate, hardware-dependent)
 *
 *   [LockFreeHashMap]
 *     LockFreeHashMap:           ~120 ns/op  throughput: ~8.3M ops/s
 *     std::mutex + unordered_map: ~680 ns/op  throughput: ~1.5M ops/s
 *     Speedup: 5.6×
 *
 *   [IntrusiveList]
 *     IntrusiveList (zero-alloc): ~12 ns/op
 *     std::list (heap):          ~180 ns/op
 *     Speedup: 15×
 *
 *   [GenerationPool]
 *     GenerationPool acquire/release: ~18 ns/op
 *     make_shared / reset:            ~95 ns/op
 *     Speedup: 5.3×
 */

#include <qbuem/buf/generation_pool.hpp>
#include <qbuem/buf/intrusive_list.hpp>
#include <qbuem/buf/lock_free_hash_map.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <list>
#include <memory>
#include <mutex>
#include <qbuem/compat/print.hpp>
#include <random>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std::chrono_literals;

using Clock    = std::chrono::steady_clock;
using Duration = std::chrono::nanoseconds;

// ── Benchmark utilities ───────────────────────────────────────────────────────

/**
 * @brief Run `fn` for at least `target_duration` and return throughput stats.
 */
struct BenchResult {
    double  ns_per_op;
    double  ops_per_sec;
    int64_t total_ops;
};

template <typename Fn>
[[nodiscard]] static BenchResult bench(std::string_view /*label*/,
                                       std::chrono::milliseconds duration,
                                       Fn&& fn) {
    int64_t ops = 0;
    const auto start = Clock::now();
    const auto end   = start + duration;

    while (Clock::now() < end) {
        fn();
        ++ops;
    }

    const auto elapsed_ns =
        std::chrono::duration_cast<Duration>(Clock::now() - start).count();
    const double ns_per_op  = static_cast<double>(elapsed_ns) / static_cast<double>(ops);
    const double ops_per_sec = 1e9 / ns_per_op;
    return {ns_per_op, ops_per_sec, ops};
}

// ── Benchmark 1: Hash map ─────────────────────────────────────────────────────

static void bench_hash_map() {
    constexpr size_t kCapacity = 4096;
    constexpr size_t kKeys     = 1024;

    std::println("\n[1] Hash Map: LockFreeHashMap vs. std::mutex + std::unordered_map");
    std::println("    Workload: {:} keys, mixed put/get (75 % get, 25 % put)", kKeys);

    // Build a random key set (no key 0 — it is reserved).
    std::mt19937_64 rng(42);
    std::vector<uint64_t> keys(kKeys);
    std::ranges::generate(keys, [&] {
        uint64_t k;
        do { k = rng(); } while (k == 0);
        return k;
    });

    // Pre-populate both maps with half the keys.
    qbuem::LockFreeHashMap<uint64_t, uint32_t> lf_map(kCapacity);
    std::unordered_map<uint64_t, uint32_t>     std_map;
    std::mutex                                  std_mtx;

    for (size_t i = 0; i < keys.size() / 2; ++i) {
        lf_map.put(keys[i], static_cast<uint32_t>(i));
        std_map.emplace(keys[i], static_cast<uint32_t>(i));
    }

    std::atomic<uint32_t> key_idx{0};

    // LockFreeHashMap benchmark
    const auto lf_res = bench("lf_map", 500ms, [&] {
        const uint32_t i   = key_idx.fetch_add(1, std::memory_order_relaxed) % kKeys;
        const uint64_t key = keys[i];
        if ((i & 3u) == 0u) {
            lf_map.put(key, i);
        } else {
            auto v = lf_map.get(key);
            (void)v;
        }
    });

    key_idx.store(0);

    // std::mutex + unordered_map benchmark
    const auto std_res = bench("std_map", 500ms, [&] {
        const uint32_t i   = key_idx.fetch_add(1, std::memory_order_relaxed) % kKeys;
        const uint64_t key = keys[i];
        std::lock_guard lock(std_mtx);
        if ((i & 3u) == 0u) {
            std_map[key] = i;
        } else {
            auto it = std_map.find(key);
            (void)it;
        }
    });

    std::println("    LockFreeHashMap:            {:>8.1f} ns/op  {:>10.0f} ops/s",
                 lf_res.ns_per_op, lf_res.ops_per_sec);
    std::println("    std::mutex+unordered_map:   {:>8.1f} ns/op  {:>10.0f} ops/s",
                 std_res.ns_per_op, std_res.ops_per_sec);
    std::println("    Speedup: {:.1f}×",
                 std_res.ns_per_op / lf_res.ns_per_op);
}

// ── Benchmark 2: Intrusive list ───────────────────────────────────────────────

struct Task : public qbuem::IntrusiveNode {
    int priority{0};
    int id{0};
};

static void bench_intrusive_list() {
    constexpr size_t kNodes = 256;

    std::println("\n[2] List: IntrusiveList vs. std::list");
    std::println("    Workload: push_back + pop_front cycle, {} nodes in flight", kNodes);

    // Pre-allocate nodes (IntrusiveList is zero-allocation — nodes are external).
    alignas(64) Task task_pool[kNodes];
    for (size_t i = 0; i < kNodes; ++i) {
        task_pool[i].priority = static_cast<int>(i);
        task_pool[i].id       = static_cast<int>(i);
    }

    // IntrusiveList benchmark
    qbuem::IntrusiveList<Task> il;
    size_t node_idx = 0;

    const auto il_res = bench("intrusive_list", 500ms, [&] {
        Task* t = &task_pool[node_idx % kNodes];
        if (!t->linked()) {
            il.push_back(t);
            node_idx = (node_idx + 1) % kNodes;
        } else if (!il.empty()) {
            Task* popped = il.pop_front();
            (void)popped;
        }
    });

    // Drain before next benchmark
    while (!il.empty()) il.pop_front();

    // std::list benchmark
    std::list<int> sl;

    const auto sl_res = bench("std_list", 500ms, [&] {
        sl.push_back(static_cast<int>(node_idx++ % kNodes));
        if (!sl.empty()) {
            sl.pop_front();
        }
    });

    std::println("    IntrusiveList (zero-alloc): {:>8.1f} ns/op  {:>10.0f} ops/s",
                 il_res.ns_per_op, il_res.ops_per_sec);
    std::println("    std::list (heap-alloc):     {:>8.1f} ns/op  {:>10.0f} ops/s",
                 sl_res.ns_per_op, sl_res.ops_per_sec);
    std::println("    Speedup: {:.1f}×",
                 sl_res.ns_per_op / il_res.ns_per_op);
}

// ── Benchmark 3: Object pool ──────────────────────────────────────────────────

struct Event {
    uint64_t timestamp{0};
    uint32_t type{0};
    uint32_t payload[4]{};
};

static void bench_generation_pool() {
    constexpr size_t kPoolSize = 512;

    std::println("\n[3] Object Pool: GenerationPool vs. shared_ptr");
    std::println("    Workload: acquire→use→release cycle, pool of {} slots", kPoolSize);

    // GenerationPool benchmark
    qbuem::GenerationPool<Event> pool(kPoolSize);
    std::vector<qbuem::GenerationHandle> handles;
    handles.reserve(32);

    const auto gp_res = bench("gen_pool", 500ms, [&] {
        // Acquire a slot, "use" it, release.
        if (auto ar = pool.acquire()) {
            new (ar->ptr) Event{.timestamp = 42, .type = 1};
            handles.push_back(ar->handle);
        }
        if (!handles.empty()) {
            Event* e = pool.resolve(handles.back());
            if (e) { volatile uint64_t ts = e->timestamp; (void)ts; }
            pool.release(handles.back());
            handles.pop_back();
        }
    });

    // shared_ptr benchmark
    std::vector<std::shared_ptr<Event>> sp_vec;
    sp_vec.reserve(32);

    const auto sp_res = bench("shared_ptr", 500ms, [&] {
        sp_vec.push_back(std::make_shared<Event>(Event{.timestamp = 42, .type = 1}));
        if (sp_vec.size() > 16) {
            volatile uint64_t ts = sp_vec.back()->timestamp;
            (void)ts;
            sp_vec.pop_back();
        }
    });

    std::println("    GenerationPool acquire/release: {:>8.1f} ns/op  {:>10.0f} ops/s",
                 gp_res.ns_per_op, gp_res.ops_per_sec);
    std::println("    make_shared / use / reset:      {:>8.1f} ns/op  {:>10.0f} ops/s",
                 sp_res.ns_per_op, sp_res.ops_per_sec);
    std::println("    Speedup: {:.1f}×",
                 sp_res.ns_per_op / gp_res.ns_per_op);
}

// ── Benchmark 4: Multi-thread hash map contention ────────────────────────────

static void bench_concurrent_hash_map() {
    constexpr size_t   kCapacity = 8192;
    constexpr size_t   kKeys     = 2048;
    constexpr unsigned kThreads  = 4;
    constexpr auto     kDuration = 500ms;

    std::println("\n[4] Concurrent Hash Map: {} threads, mixed workload", kThreads);

    std::mt19937_64 rng(99);
    std::vector<uint64_t> keys(kKeys);
    std::ranges::generate(keys, [&] {
        uint64_t k;
        do { k = rng(); } while (k == 0);
        return k;
    });

    // LockFreeHashMap — no mutex needed
    qbuem::LockFreeHashMap<uint64_t, uint32_t> lf_map(kCapacity);
    for (size_t i = 0; i < keys.size() / 2; ++i)
        lf_map.put(keys[i], static_cast<uint32_t>(i));

    std::atomic<int64_t> lf_ops{0};
    {
        std::vector<std::jthread> threads;
        for (unsigned t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                const auto end = Clock::now() + kDuration;
                int64_t local_ops = 0;
                std::mt19937_64 local_rng(t * 1234567ULL);
                while (Clock::now() < end) {
                    const size_t i   = local_rng() % kKeys;
                    const uint64_t k = keys[i];
                    if ((local_rng() & 3u) == 0u) lf_map.put(k, static_cast<uint32_t>(i));
                    else                           { auto v = lf_map.get(k); (void)v; }
                    ++local_ops;
                }
                lf_ops.fetch_add(local_ops, std::memory_order_relaxed);
            });
        }
    }

    // std::mutex + unordered_map — needs lock
    std::unordered_map<uint64_t, uint32_t> std_map;
    std::mutex                              std_mtx;
    for (size_t i = 0; i < keys.size() / 2; ++i)
        std_map.emplace(keys[i], static_cast<uint32_t>(i));

    std::atomic<int64_t> std_ops{0};
    {
        std::vector<std::jthread> threads;
        for (unsigned t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                const auto end = Clock::now() + kDuration;
                int64_t local_ops = 0;
                std::mt19937_64 local_rng(t * 9999991ULL);
                while (Clock::now() < end) {
                    const size_t   i = local_rng() % kKeys;
                    const uint64_t k = keys[i];
                    std::lock_guard lock(std_mtx);
                    if ((local_rng() & 3u) == 0u) std_map[k] = static_cast<uint32_t>(i);
                    else                           { auto it = std_map.find(k); (void)it; }
                    ++local_ops;
                }
                std_ops.fetch_add(local_ops, std::memory_order_relaxed);
            });
        }
    }

    const double elapsed_s = kDuration.count() / 1000.0;
    const double lf_ops_s  = static_cast<double>(lf_ops.load()) / elapsed_s;
    const double std_ops_s = static_cast<double>(std_ops.load()) / elapsed_s;

    std::println("    LockFreeHashMap ({} threads): {:>12.0f} total ops/s", kThreads, lf_ops_s);
    std::println("    std::mutex+map  ({} threads): {:>12.0f} total ops/s", kThreads, std_ops_s);
    std::println("    Speedup: {:.1f}×  (lock contention collapses throughput under load)",
                 lf_ops_s / std_ops_s);
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main() {
    std::println("=== qbuem-stack Lock-Free Primitive Benchmark ===");
    std::println("Each benchmark runs for 500 ms per variant.\n");

    bench_hash_map();
    bench_intrusive_list();
    bench_generation_pool();
    bench_concurrent_hash_map();

    std::println("\n=== Key Takeaways ===");
    std::println("  LockFreeHashMap:   eliminates mutex contention; O(1) wait-free reads.");
    std::println("  IntrusiveList:     zero heap allocation; O(1) insert/remove.");
    std::println("  GenerationPool:    ABA-safe handles; reuses fixed slots without malloc.");
    std::println("  All three satisfy qbuem's Pillar 3 (Zero Allocation) on the hot path.");
    return 0;
}
