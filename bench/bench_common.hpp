#pragma once

/**
 * @file bench/bench_common.hpp
 * @brief Common benchmark utilities for qbuem-stack.
 *
 * ### Design Principles
 * - No external dependencies (std::chrono + POSIX only)
 * - Compiler optimization prevention: DoNotOptimize()
 * - Statistics: avg, min, max, throughput (ops/s, MB/s)
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <numeric>
#include <print>
#include <string>
#include <vector>

namespace bench {

// ─── Compiler optimization prevention ────────────────────────────────────────

template <typename T>
inline void do_not_optimize(T const& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}

template <typename T>
inline void do_not_optimize(T& value) {
    asm volatile("" : "+r,m"(value) : : "memory");
}

inline void clobber_memory() {
    asm volatile("" : : : "memory");
}

// ─── Timing utilities ─────────────────────────────────────────────────────────

using Clock = std::chrono::steady_clock;
using Nanos = std::chrono::nanoseconds;

inline uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<Nanos>(Clock::now().time_since_epoch()).count());
}

// ─── Benchmark result ─────────────────────────────────────────────────────────

struct Result {
    std::string name;
    uint64_t    iterations;
    double      total_ns;
    double      min_ns;
    double      max_ns;

    double avg_ns()  const { return total_ns / static_cast<double>(iterations); }
    double ops_per_sec() const {
        return static_cast<double>(iterations) / (total_ns * 1e-9);
    }
    double mb_per_sec(size_t bytes_per_iter) const {
        return (static_cast<double>(iterations) * static_cast<double>(bytes_per_iter))
               / (total_ns * 1e-9) / (1024.0 * 1024.0);
    }

    void print() const {
        std::print("  {:<40}  {:8.1f} ns/op  {:10.0f} ops/s  [min={:6.1f} max={:8.1f}]\n",
               name, avg_ns(), ops_per_sec(), min_ns, max_ns);
    }

    void print_throughput(size_t bytes_per_iter) const {
        std::print("  {:<40}  {:8.1f} ns/op  {:10.0f} ops/s  {:7.1f} MB/s  [min={:6.1f} max={:8.1f}]\n",
               name, avg_ns(), ops_per_sec(),
               mb_per_sec(bytes_per_iter), min_ns, max_ns);
    }
};

// ─── Benchmark runner ─────────────────────────────────────────────────────────

/**
 * @brief Run a single benchmark.
 *
 * @param name         Benchmark name
 * @param warmup_iter  Warmup iteration count (JIT, branch prediction prep)
 * @param measure_iter Measurement iteration count
 * @param fn           Benchmark function (1 iteration)
 */
Result run(const std::string& name,
           uint64_t warmup_iter,
           uint64_t measure_iter,
           std::function<void()> fn) {
    // Warmup
    for (uint64_t i = 0; i < warmup_iter; ++i) {
        fn();
        clobber_memory();
    }

    // Measure
    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(measure_iter));

    for (uint64_t i = 0; i < measure_iter; ++i) {
        const uint64_t t0 = now_ns();
        fn();
        clobber_memory();
        const uint64_t t1 = now_ns();
        samples.push_back(static_cast<double>(t1 - t0));
    }

    double total = std::accumulate(samples.begin(), samples.end(), 0.0);
    double mn    = *std::min_element(samples.begin(), samples.end());
    double mx    = *std::max_element(samples.begin(), samples.end());

    return Result{name, measure_iter, total, mn, mx};
}

/**
 * @brief Run a batch benchmark — fn() performs N operations per call.
 *
 * @param name          Benchmark name
 * @param batch_size    Number of items processed per fn() call
 * @param warmup_runs   Warmup fn() call count
 * @param measure_runs  Measurement fn() call count
 * @param fn            Batch processing function
 */
Result run_batch(const std::string& name,
                 uint64_t           batch_size,
                 uint64_t           warmup_runs,
                 uint64_t           measure_runs,
                 std::function<void()> fn) {
    for (uint64_t i = 0; i < warmup_runs; ++i) { fn(); clobber_memory(); }

    const uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < measure_runs; ++i) { fn(); clobber_memory(); }
    const uint64_t t1 = now_ns();

    const double total_ns   = static_cast<double>(t1 - t0);
    const double total_ops  = static_cast<double>(measure_runs * batch_size);
    const double ns_per_op  = total_ns / total_ops;

    return Result{name, static_cast<uint64_t>(total_ops), total_ns, ns_per_op, ns_per_op};
}

// ─── Section header ───────────────────────────────────────────────────────────

inline void section(const char* title) {
    std::println("\n╔══════════════════════════════════════════════════════════╗");
    std::print("║  {:<57}║\n", title);
    std::println("╚══════════════════════════════════════════════════════════╝");
}

inline void pass(const char* msg) {
    std::println("  \033[32m✓\033[0m {}", msg);
}

inline void fail(const char* msg) {
    std::println("  \033[31m✗\033[0m {}", msg);
}

} // namespace bench
