# qbuem-stack Benchmarks

A collection of micro-benchmarks built with `-O3 -march=native`, measuring
performance as it would appear in a production release build.

## Build

```sh
# From the repository root
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target bench_http bench_router bench_arena bench_channel bench_pipeline bench_grid -j$(nproc)
```

Or build everything at once:

```sh
cmake --build build -j$(nproc)
```

## Run Individual Benchmarks

```sh
# HTTP parser throughput (MB/s)
./build/bench/bench_http

# Router lookup latency (ns/req)
./build/bench/bench_router

# Arena allocation speed (ns/alloc)
./build/bench/bench_arena

# AsyncChannel / SpscChannel throughput (ops/s)
./build/bench/bench_channel

# 3-stage pipeline throughput (items/s)
./build/bench/bench_pipeline

# GridBitset spatial queries + TiledBitset dynamic world (ns/op)
./build/bench/bench_grid
```

## Performance Targets (v3.4.0)

### Core Components

| Benchmark | Metric | Target | Notes |
|-----------|--------|--------|-------|
| HTTP parser | Throughput | > 300 MB/s | AVX2 path; ~1 GiB/s with AVX-512 |
| Router lookup | Latency | < 200 ns/req | 1,000-route Radix Tree |
| Arena alloc | Latency | < 5 ns/alloc | Bump-pointer, zero heap allocation |
| AsyncChannel | Throughput | > 40M ops/s | MPMC ring-buffer |
| SpscChannel | Throughput | > 100M ops/s | Wait-free single-producer/consumer |
| Pipeline | Throughput | > 1M items/s | 3-stage coroutine chain |

### GridBitset Spatial Queries

> 256×256 grid, 32 layers, ~10 % density. Single-threaded.

| Benchmark | Target | Notes |
|-----------|--------|-------|
| `test(x,y,layer)` | < 30 ns | Atomic load, L3-bound at 256×256 |
| `any_in_box 8×8` | < 50 ns | 64-cell SIMD scan |
| `any_in_radius r=20` | < 100 ns | Per-row sqrt extent + AVX2/NEON scan |
| `count_in_radius r=20` | < 200 ns | Full circular area scan |
| `raycast` 32-step | < 50 ns | Bresenham DDA, early-exit on hit |
| `for_each_set` sparse | < 1 ns/cell | Sequential, L1-hot |

### TiledBitset Dynamic World

> 256×256 tiles, 16 layers, 25 pre-loaded tiles, ~10 % density.

| Benchmark | Target | Notes |
|-----------|--------|-------|
| `set(wx,wy,layer)` | < 50 ns | TLS cache hit → direct tile write |
| `test(wx,wy,layer)` | < 50 ns | 4-slot TLS cache; shared_mutex on miss |
| `any_in_box` cross-tile | < 200 ns | Splits scan at tile boundaries |
| `any_in_radius r=50` | < 500 ns | Cross-tile per-row SIMD scan |
| `raycast` 400-step | < 5 µs | Bresenham DDA across multiple tiles |

## Reading Results (p50 / p99)

Benchmark output typically includes:

```
ops/sec   : Throughput — higher is better.
p50 (ns)  : Median latency — typical case performance.
p99 (ns)  : 99th-percentile latency — tail latency.
            Use p99 as the primary SLO design target.
p999 (ns) : 99.9th-percentile — measures extreme outliers.
```

### Interpretation Guide

- **Low p50 + high p99**: Tail-latency problem. Suspect lock contention, OS
  scheduler jitter, or NUMA cross-traffic. Profile with `perf stat -e cs`.
- **High p50**: Hot-path bottleneck. Use `perf record -g` or VTune to find the
  hot instruction.
- **Throughput below target**: Check CPU core count, NUMA topology, and channel
  capacity (`channel_cap`). Ensure `SO_BUSY_POLL` / `io_uring` is in use.

## Recommended Measurement Environment

```
CPU:      Intel Xeon Gold 6254 @ 3.10GHz (18C/36T)  — or equivalent
OS:       Ubuntu 22.04 LTS, kernel 5.15+
Compiler: GCC 13+, Clang 17+  |  Flags: -O3 -march=native
Build:    Release (cmake -DCMAKE_BUILD_TYPE=Release)
Isolate:  taskset -c 0 ./bench_xxx    # pin to a single core
```

For reproducible results:

```sh
# Fix CPU frequency to performance governor
sudo cpupower frequency-set -g performance

# Pin to a single isolated core
taskset -c 0 ./build/bench/bench_http

# Minimize background load
sudo systemctl stop snapd.service bluetooth.service
```
