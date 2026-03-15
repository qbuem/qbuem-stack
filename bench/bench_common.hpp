#pragma once

/**
 * @file bench/bench_common.hpp
 * @brief qbuem-stack 성능 벤치마크 공통 유틸리티.
 *
 * ### 설계 원칙
 * - 외부 의존성 없음 (std::chrono + POSIX만 사용)
 * - 컴파일러 최적화 방지: DoNotOptimize() 사용
 * - 통계: 평균, 최소, 최대, 처리량(ops/s, MB/s) 출력
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <numeric>
#include <string>
#include <vector>

namespace bench {

// ─── 컴파일러 최적화 방지 ────────────────────────────────────────────────────

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

// ─── 타이밍 유틸리티 ─────────────────────────────────────────────────────────

using Clock = std::chrono::steady_clock;
using Nanos = std::chrono::nanoseconds;

inline uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<Nanos>(Clock::now().time_since_epoch()).count());
}

// ─── 벤치마크 결과 ───────────────────────────────────────────────────────────

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
        printf("  %-40s  %8.1f ns/op  %10.0f ops/s  [min=%6.1f max=%8.1f]\n",
               name.c_str(), avg_ns(), ops_per_sec(), min_ns, max_ns);
    }

    void print_throughput(size_t bytes_per_iter) const {
        printf("  %-40s  %8.1f ns/op  %10.0f ops/s  %7.1f MB/s  [min=%6.1f max=%8.1f]\n",
               name.c_str(), avg_ns(), ops_per_sec(),
               mb_per_sec(bytes_per_iter), min_ns, max_ns);
    }
};

// ─── 벤치마크 실행기 ─────────────────────────────────────────────────────────

/**
 * @brief 단일 벤치마크 실행.
 *
 * @param name        벤치마크 이름
 * @param warmup_iter 워밍업 반복 횟수 (JIT, 분기 예측 준비)
 * @param measure_iter 측정 반복 횟수
 * @param fn          벤치마크 함수 (1 iteration)
 */
Result run(const std::string& name,
           uint64_t warmup_iter,
           uint64_t measure_iter,
           std::function<void()> fn) {
    // 워밍업
    for (uint64_t i = 0; i < warmup_iter; ++i) {
        fn();
        clobber_memory();
    }

    // 측정
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
 * @brief 배치 벤치마크 실행 — fn()이 N번 작업을 수행.
 *
 * @param name         벤치마크 이름
 * @param batch_size   fn() 한 번 호출 시 처리하는 아이템 수
 * @param warmup_runs  워밍업 fn() 호출 횟수
 * @param measure_runs 측정 fn() 호출 횟수
 * @param fn           배치 처리 함수
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

// ─── 섹션 헤더 ───────────────────────────────────────────────────────────────

inline void section(const char* title) {
    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║  %-57s║\n", title);
    printf("╚══════════════════════════════════════════════════════════╝\n");
}

inline void pass(const char* msg) {
    printf("  \033[32m✓\033[0m %s\n", msg);
}

inline void fail(const char* msg) {
    printf("  \033[31m✗\033[0m %s\n", msg);
}

} // namespace bench
