// ============================================================================
// qbuem-stack — Callback allocation benchmark
//
// Demonstrates the hot-path cost of std::function (per-construction heap
// allocation for any closure larger than its ~16-byte small-buffer
// optimization) versus qbuem::inplace_function (a fixed inline buffer, zero
// allocation). This is the Zero-Allocation pillar made measurable: reactor I/O
// callbacks and pipeline stage closures are constructed on per-event / per-
// message paths, so the std::function malloc is a hidden, recurring cost.
//
// The callback is forced to ESCAPE through a [[gnu::noinline]] sink so the
// optimizer cannot elide the allocation the way it does in a trivial loop.
// ============================================================================
#include <qbuem/buf/inplace_function.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>

static std::atomic<std::size_t> g_allocs{0};
void* operator new(std::size_t n) { g_allocs.fetch_add(1, std::memory_order_relaxed); return std::malloc(n ? n : 1); }
void  operator delete(void* p) noexcept { std::free(p); }
void  operator delete(void* p, std::size_t) noexcept { std::free(p); }

std::uint64_t g_keep = 0;
[[gnu::noinline]] static void consume_std(std::function<std::uint64_t()>& f) { g_keep += f(); }
[[gnu::noinline]] static void consume_ipf(qbuem::inplace_function<std::uint64_t(), 64>& f) { g_keep += f(); }

int main() {
    constexpr int N = 2'000'000;
    std::array<std::uint64_t, 4> cap{1, 2, 3, 4};  // 32-byte capture (> std::function SBO)

    std::puts("\n  qbuem-stack — Callback Allocation Benchmark");
    std::puts("  (32-byte capture, escaping store, 2M constructions)\n");

    g_allocs = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        std::function<std::uint64_t()> f([cap, i]() { return cap[0] + cap[3] + (std::uint64_t)i; });
        consume_std(f);
    }
    auto t1 = std::chrono::steady_clock::now();
    const std::size_t std_allocs = g_allocs.load();
    const double std_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / N;

    g_allocs = 0;
    auto t2 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        qbuem::inplace_function<std::uint64_t(), 64> f(
            [cap, i]() { return cap[0] + cap[3] + (std::uint64_t)i; });
        consume_ipf(f);
    }
    auto t3 = std::chrono::steady_clock::now();
    const std::size_t ipf_allocs = g_allocs.load();
    const double ipf_ns = std::chrono::duration<double, std::nano>(t3 - t2).count() / N;

    std::printf("  %-26s %7.2f ns/op   heap allocs/op: %.2f\n",
                "std::function", std_ns, (double)std_allocs / N);
    std::printf("  %-26s %7.2f ns/op   heap allocs/op: %.2f\n",
                "qbuem::inplace_function", ipf_ns, (double)ipf_allocs / N);
    if (ipf_allocs == 0 && std_allocs > 0)
        std::printf("\n  \xE2\x9C\x93 inplace_function: %.1fx faster, zero allocation\n",
                    std_ns / ipf_ns);
    std::printf("  (keep=%llu)\n", (unsigned long long)g_keep);
    return 0;
}
