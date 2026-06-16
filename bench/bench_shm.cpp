/**
 * @file bench/bench_shm.cpp
 * @brief SHMChannel<T> throughput benchmarks (shared-memory MPMC channel).
 *
 * SHMChannel is the cross-process IPC primitive; these run it in-process (the
 * segment is mmap'd and usable from the same process) to measure the lock-free
 * Vyukov ring's raw cost: single-thread round-trip, batch fill+drain, and a
 * true 2-thread producer→consumer throughput.
 */
#include "bench_common.hpp"

#include <qbuem/shm/shm_channel.hpp>

#include <cstdint>
#include <string>
#include <thread>
#include <qbuem/compat/print.hpp>

using namespace qbuem::shm;

namespace {

// 32-byte trivially-copyable payload (SHMChannel requires trivial copyability).
struct Msg {
    uint64_t a, b, c, d;
};
static_assert(std::is_trivially_copyable_v<Msg>);

void bench_roundtrip() {
    bench::section("SHMChannel<Msg> — try_send + try_recv round-trip");
    auto ch = SHMChannel<Msg>::create("qbuem_bench_rt", 4096);
    if (!ch) {
        std::println("  (skipped — SHMChannel::create failed; /dev/shm unavailable?)");
        return;
    }
    auto& chan = *ch;
    const Msg m{1, 2, 3, 4};

    auto res = bench::run_batch(
        "SHMChannel: try_send + try_recv", 1, 1'000'000, 10'000'000, [&] {
            bool ok = chan->try_send(m);
            bench::do_not_optimize(ok);
            auto v = chan->try_recv();
            bench::do_not_optimize(v);
        });
    res.print_throughput(sizeof(Msg));
    SHMChannel<Msg>::unlink("qbuem_bench_rt");
}

void bench_fill_drain() {
    bench::section("SHMChannel<Msg> — fill N then drain N (batch)");
    constexpr size_t kCap = 8192;
    auto ch = SHMChannel<Msg>::create("qbuem_bench_fd", kCap);
    if (!ch) {
        std::println("  (skipped — SHMChannel::create failed)");
        return;
    }
    auto& chan = *ch;
    const Msg m{5, 6, 7, 8};

    auto res = bench::run_batch(
        "SHMChannel: fill 4096 + drain 4096", 4096, 2000, 20000, [&] {
            for (size_t i = 0; i < 4096; ++i) bench::do_not_optimize(chan->try_send(m));
            for (size_t i = 0; i < 4096; ++i) { auto v = chan->try_recv(); bench::do_not_optimize(v); }
        });
    res.print_throughput(sizeof(Msg));
    SHMChannel<Msg>::unlink("qbuem_bench_fd");
}

void bench_2thread() {
    bench::section("SHMChannel<Msg> — 2-thread producer→consumer throughput");
    auto ch = SHMChannel<Msg>::create("qbuem_bench_2t", 4096);
    if (!ch) {
        std::println("  (skipped — SHMChannel::create failed)");
        return;
    }
    auto& chan = *ch;
    constexpr size_t kItems = 5'000'000;

    const uint64_t t0 = bench::now_ns();
    std::jthread producer([&] {
        const Msg m{9, 10, 11, 12};
        for (size_t i = 0; i < kItems; ++i)
            while (!chan->try_send(m)) { /* spin until a slot frees */ }
    });

    size_t received = 0;
    while (received < kItems) {
        auto v = chan->try_recv();
        if (v) ++received;
    }
    producer.join();
    const uint64_t t1 = bench::now_ns();

    const double total_ns = static_cast<double>(t1 - t0);
    const double ops_s    = static_cast<double>(kItems) / (total_ns * 1e-9);
    const double ns_op    = total_ns / static_cast<double>(kItems);
    const double mb_s     = static_cast<double>(kItems) * sizeof(Msg)
                            / (total_ns * 1e-9) / (1024.0 * 1024.0);
    std::print("  {:<40}  {:8.2f} ns/op  {:10.0f} ops/s  {:7.1f} MB/s\n",
               "SHMChannel: 2-thread P->C", ns_op, ops_s, mb_s);
    SHMChannel<Msg>::unlink("qbuem_bench_2t");
}

} // namespace

int main() {
    std::println();
    std::println("══════════════════════════════════════════════════════════════");
    std::println("  qbuem-stack — SHM Channel Performance Benchmark");
    std::println("══════════════════════════════════════════════════════════════");

    bench_roundtrip();
    bench_fill_drain();
    bench_2thread();

    std::println();
    std::println("══════════════════════════════════════════════════════════════");
    std::println("  Done");
    std::println("══════════════════════════════════════════════════════════════");
    std::println();
    return 0;
}
