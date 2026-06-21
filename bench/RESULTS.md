# Benchmark Results

Reproducible micro-benchmark numbers for qbuem-stack. **These are real measured
values, not targets.** Reproduce them yourself with the commands below; numbers
scale with your CPU.

> The per-benchmark `✓`/`✗` "goals" printed by the binaries are aspirational
> targets originally calibrated on x86 server hardware (Xeon-class). On other
> CPUs some will read `✗` and that is expected — treat the numbers, not the
> goal flags, as the result. CI runs the benchmarks for **build+run health**
> (regressions/crashes surface), not to assert these thresholds.

> **Live CI numbers.** Every push runs the `Benchmarks / linux-x86_64` and
> `Benchmarks / linux-aarch64` jobs, which **print the full measured numbers in
> the run's Job Summary** and upload them as `benchmarks-<arch>` artifacts — so
> current per-architecture **server** baselines (x86_64 + aarch64) are always
> available on the Actions run page, alongside the local laptop baseline below.

## How to reproduce

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DQBUEM_BUILD_BENCH=ON
cmake --build build -j$(nproc) --target \
  bench_arena bench_callback bench_channel bench_crypto bench_grid \
  bench_http bench_pipeline bench_router bench_shm
for b in arena callback channel crypto grid http pipeline router shm; do
  ./build/bench/bench_$b
done
```

---

## Baseline — Apple M1 Pro

| Field | Value |
|---|---|
| CPU | Apple M1 Pro (ARM64) |
| OS | macOS (Darwin 25.x) |
| Compiler | Apple clang, C++23 |
| Build | `Release` (`-O3`) |
| Crypto | software path (no `QBUEM_ENABLE_NATIVE_CRYPTO`; ARM AES-GCM skipped) |

### Memory (`bench_arena`, `bench_callback`)

| Operation | Result |
|---|---|
| `Arena` bump-alloc 64 B | **6.7 ns/op** (~148 M ops/s) |
| `Arena` request lifecycle (10 alloc + reset) | **4.5 ns/op** |
| `Arena` mixed-size (8–1024 B) | **3.9 ns/op** |
| `FixedPoolResource` alloc+dealloc 256 B | **5.1 ns/op** |
| `malloc`/`free` 64 B (baseline) | 28.8 ns/op |
| `inplace_function` call (32 B capture) | **2.4 ns/op, 0 heap allocs** |
| `std::function` call (same) | 28.6 ns/op, 1 heap alloc/op |

### Channels (`bench_channel`, `bench_shm`)

| Operation | Result |
|---|---|
| `AsyncChannel` try_send+try_recv round-trip | **7.6 ns/op** (~132 M ops/s) |
| `AsyncChannel` fill 1000 + drain 1000 | **3.3 ns/op** (~305 M ops/s, ~1.16 GB/s) |
| `SpscChannel` try_send+try_recv round-trip | 10.4 ns/op (~96 M ops/s) |
| `SpscChannel` 2-thread cross-core | 29.8 ns/op (~33 M items/s) |
| `ArenaChannel` push+pop round-trip | **5.0 ns/op** (~199 M ops/s) |
| `SHMChannel` try_send+try_recv | **10.4 ns/op** (~96 M ops/s, ~2.9 GB/s) |
| `SHMChannel` 2-thread P→C | 41.2 ns/op (~24 M items/s) |

### HTTP & routing (`bench_http`, `bench_router`)

| Operation | Result |
|---|---|
| HTTP parse — simple GET (74 B) | **192 ns/op** (~367 MB/s) |
| HTTP parse — POST + 10 headers (310 B) | 527 ns/op (~561 MB/s) |
| HTTP parse — chunked POST (98 B) | 190 ns/op (~491 MB/s) |
| Router — static lookup | **60 ns/op** |
| Router — single param (`/users/:id`) | 74 ns/op |
| Router — double param | 121 ns/op |
| Router — 1100-route table (hit) | 82 ns/op |

### Pipeline / IO primitives (`bench_pipeline`)

| Operation | Result |
|---|---|
| `Context::get<T>()` | ~18 ns/op |
| `ServiceRegistry::get<T>()` | 36 ns/op |
| `IOSlice` create + `to_iovec()` | **0.3 ns/op** |
| `IOVec<4>` 4× push (scatter-gather) | **0.9 ns/op** |

### Crypto (`bench_crypto`, software path)

| Operation | Result |
|---|---|
| SHA-256, 16 KiB | ~192 MB/s |
| HMAC-SHA-256, 16 KiB | ~190 MB/s |
| ChaCha20-Poly1305 seal, 16 KiB | ~637 MB/s |
| ChaCha20-Poly1305 open, 16 KiB | ~638 MB/s |
| AES-256-GCM | skipped (build with `-DQBUEM_ENABLE_NATIVE_CRYPTO=ON` for the hardware path; ~11× SHA-256) |

> SHA-256 is the software scalar path here. With `QBUEM_ENABLE_NATIVE_CRYPTO=ON`
> on a CPU with ARM-SHA2 / SHA-NI, SHA-256 is roughly an order of magnitude
> faster (see CLAUDE.md / the crypto guide).

---

*Add a second machine's results below as a new `## Baseline — <CPU>` section.*
