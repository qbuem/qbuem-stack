/**
 * @file numa_hugepages_example.cpp
 * @brief NUMA scheduling + Huge Pages + CPU hints + I/O slice example.
 *
 * ## Coverage
 * - pin_reactor_to_cpu()        — Pin Reactor to a specific CPU
 * - auto_numa_bind()            — Auto-assign Reactors per NUMA node
 * - numa_node_count()           — Detect number of NUMA nodes
 * - cpu_to_numa_map()           — CPU → NUMA node mapping
 * - PerfCounters                — PMU hardware performance counters
 * - HugeBufferPool<N, Count>    — huge page mmap buffer pool (acquire/release)
 * - prefetch_read<Locality>()   — read prefetch
 * - prefetch_write<Locality>()  — write prefetch
 * - prefetch_ahead<T, Ahead>()  — prefetch ahead on array of structs
 * - kCacheLineSize              — cache line size constant
 * - compiler_barrier()          — compiler memory barrier
 * - cpu_pause()                 — spin-wait pause hint
 * - IOSlice                     — read-only scatter-gather slice
 * - MutableIOSlice              — writable scatter-gather slice
 * - BufferPool<BufSize, Count>  — lock-free general-purpose buffer pool
 */

#include <qbuem/core/cpu_hints.hpp>
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/huge_pages.hpp>
#include <qbuem/core/numa.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/io/buffer_pool.hpp>
#include <qbuem/io/io_slice.hpp>

#include <cassert>
#include <cstring>
#include <thread>
#include <vector>
#include <qbuem/compat/print.hpp>

using namespace qbuem;
using namespace std::chrono_literals;
using std::println;
using std::print;

// ─────────────────────────────────────────────────────────────────────────────
// §1  CPU hints — Prefetch & Cache-line
// ─────────────────────────────────────────────────────────────────────────────

struct alignas(kCacheLineSize) ReactorState {
    std::atomic<uint64_t> processed{0};
    std::atomic<uint64_t> errors{0};
    // Padding: prevent false sharing with the next cache line
    char _pad[kCacheLineSize - 2 * sizeof(std::atomic<uint64_t>)];
};

static void demo_cpu_hints() {
    println("── §1  CPU Hints ──");
    println("  cache line size: {} bytes", kCacheLineSize);
    println("  ReactorState size: {} bytes (cache-line aligned)",
                sizeof(ReactorState));

    // Prefetch reduces TLB/cache misses when sequentially accessing a large array
    constexpr size_t N = 1024;
    std::vector<int> data(N, 0);

    for (size_t i = 0; i < N; ++i) {
        // Pre-load the next access location into cache
        if (i + 8 < N)
            prefetch_read<3>(&data[i + 8]);
        data[i] = static_cast<int>(i * 2);
    }

    // Write prefetch: prepare output buffer
    std::vector<int> out(N);
    for (size_t i = 0; i < N; ++i) {
        if (i + 4 < N)
            prefetch_write<1>(&out[i + 4]);
        out[i] = data[i] + 1;
    }

    // prefetch_ahead: prefetch N elements ahead on an array of structs
    prefetch_ahead(data.data(), 0, N);
    println("  prefetch_ahead() done");

    // Compiler barrier & CPU pause
    compiler_barrier();
    cpu_pause();
    println("  compiler_barrier(), cpu_pause() done");

    println("  prefetch done: data[0]={}, out[0]={}\n", data[0], out[0]);
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  Huge Pages buffer pool
// ─────────────────────────────────────────────────────────────────────────────

static void demo_huge_pages() {
    println("── §2  Huge Pages Buffer Pool ──");

    // Pool of 4 × 2 MiB buffers (tries MAP_HUGETLB, falls back to MAP_ANONYMOUS)
    HugeBufferPool<2 * 1024 * 1024, 4> pool;

    // Buffer 1: acquire + use + release
    auto buf1 = pool.acquire();
    if (!buf1.empty()) {
        println("  buffer 1 acquired: {} bytes", buf1.size());
        std::memset(buf1.data(), 0xAB, 64);  // initialize first 64 bytes
        println("  buffer 1 [0]=0x{:02x} (expected: ab)",
                    static_cast<unsigned char>(buf1[0]));
    } else {
        println("  buffer 1 acquire failed (pool exhausted)");
    }

    // Buffer 2: acquire
    auto buf2 = pool.acquire();
    if (!buf2.empty())
        println("  buffer 2 acquired: {} bytes", buf2.size());

    // Check remaining pool slots (4 - 2 = 2 left)
    println("  pool remaining: {} / 4 buffers", pool.available());

    // Release
    pool.release(buf1);
    pool.release(buf2);
    println("  pool remaining after release: {} / 4 buffers\n", pool.available());
}

// ─────────────────────────────────────────────────────────────────────────────
// §3  NUMA affinity + PerfCounters
// ─────────────────────────────────────────────────────────────────────────────

static void demo_numa(Dispatcher& disp) {
    println("── §3  NUMA Affinity ──");

    // Detect NUMA node count
    int nodes = numa_node_count();
    println("  NUMA node count: {}", nodes);

    // CPU → NUMA mapping
    auto cpu_map = cpu_to_numa_map();
    println("  logical CPU count: {}", cpu_map.size());
    if (!cpu_map.empty())
        println("  CPU 0 → NUMA node {}", cpu_map[0]);

    // Attempt to pin first Reactor to CPU 0
    bool pinned = pin_reactor_to_cpu(disp, 0, 0);
    println("  Reactor[0] → CPU 0 pin: {}",
                pinned ? "success" : "unsupported (non-Linux or out of range)");

    // Auto-assign Reactors by NUMA node
    size_t bound = auto_numa_bind(disp);
    println("  auto_numa_bind() bound: {} workers", bound);

    // PerfCounters — PMU hardware performance counters
    println("\n── §3b  PerfCounters ──");
    PerfCounters perf;
    println("  PMU counters available: {}",
                perf.available() ? "yes (requires CAP_PERFMON)" : "no (no permission/hardware)");

    perf.start();
    // Workload to measure
    volatile uint64_t sum = 0;
    for (int i = 0; i < 10000; ++i) sum += static_cast<uint64_t>(i);
    auto snap = perf.stop();

    println("  cycles: {}, instructions: {}",
                static_cast<unsigned long long>(snap.cycles),
                static_cast<unsigned long long>(snap.instructions));
    println("  IPC: {:.2f}", snap.ipc());
    println("  LLC misses: {}, branch misses: {}\n",
                static_cast<unsigned long long>(snap.llc_misses),
                static_cast<unsigned long long>(snap.branch_misses));
}

// ─────────────────────────────────────────────────────────────────────────────
// §4  I/O slices & buffer pool
// ─────────────────────────────────────────────────────────────────────────────

static void demo_io_slice() {
    println("── §4  I/O Slices & General Buffer Pool ──");

    // IOSlice — read-only scatter-gather slice
    std::string part1 = "Hello, ";
    std::string part2 = "qbuem!";
    std::vector<IOSlice> slices = {
        IOSlice{reinterpret_cast<const std::byte*>(part1.data()), part1.size()},
        IOSlice{reinterpret_cast<const std::byte*>(part2.data()), part2.size()},
    };
    size_t total = 0;
    for (auto& s : slices) total += s.size;
    println("  2 IOSlices, total {} bytes", total);

    // to_buffer_view() / to_iovec() conversion
    auto bv   = slices[0].to_buffer_view();
    auto iov0 = slices[0].to_iovec();
    println("  IOSlice[0] BufferView size: {}, iovec size: {}",
                bv.size(), iov0.iov_len);

    // MutableIOSlice — writable slice
    alignas(64) std::byte buf1[16]{};
    alignas(64) std::byte buf2[16]{};
    std::vector<MutableIOSlice> mut_slices = {
        MutableIOSlice{buf1, sizeof(buf1)},
        MutableIOSlice{buf2, sizeof(buf2)},
    };
    // Copy data into the first slice
    std::memcpy(mut_slices[0].data, "QBUEM", 5);
    println("  MutableIOSlice[0]: \"{}\"",
                reinterpret_cast<const char*>(buf1));

    // as_const() — MutableIOSlice → IOSlice conversion
    IOSlice ro = mut_slices[0].as_const();
    println("  as_const() size: {}", ro.size);

    // to_iovec() on MutableIOSlice
    auto iov1 = mut_slices[1].to_iovec();
    println("  MutableIOSlice[1] iovec size: {}", iov1.iov_len);

    // BufferPool<BufSize, Count> — lock-free general buffer pool (2 template args)
    BufferPool<1024, 8> bp;   // 8 × 1 KiB buffers
    println("  BufferPool available: {} / 8", bp.available());

    auto b = bp.acquire();
    if (b) {
        println("  BufferPool buffer acquired");
        b->data[0] = std::byte{0xFF};
        b->release();   // automatically returned to pool
        println("  BufferPool buffer released");
    }
    println("  BufferPool available (after release): {} / 8\n", bp.available());
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    println("=== qbuem NUMA + Huge Pages + CPU Hints Example ===\n");

    demo_cpu_hints();
    demo_huge_pages();

    // Create Dispatcher then run NUMA binding demo
    Dispatcher disp(2);
    std::jthread t([&] { disp.run(); });

    demo_numa(disp);
    demo_io_slice();

    disp.stop();
    t.join();

    println("=== Done ===");
    return 0;
}
