# numa_hugepages

**Category:** Memory
**File:** `numa_hugepages_example.cpp`
**Complexity:** Advanced

## Overview

Demonstrates NUMA-aware CPU pinning, huge-page buffer pools, CPU prefetch hints, and I/O slice management — all of the low-level performance knobs available in qbuem-stack for maximizing memory throughput on multi-socket servers.

## Scenario

A high-frequency trading engine or AI inference server runs on a dual-socket NUMA machine. Reactor threads are pinned to NUMA node-local CPUs, buffers are backed by 2 MB huge pages to reduce TLB pressure, and I/O slices are gathered into `sendmsg`-compatible `iovec` arrays.

## Architecture Diagram

```
  NUMA Topology (example: 2 sockets)
  ──────────────────────────────────────────────────────────
  Socket 0 (NUMA node 0)           Socket 1 (NUMA node 1)
  CPUs 0–15                        CPUs 16–31
  DDR channel A                    DDR channel B
       │                                │
       │ pin_reactor_to_cpu(cpu_id)     │
       ▼                                ▼
  Reactor thread 0                 Reactor thread 1
  (NUMA-local memory access)       (NUMA-local memory access)

  HugeBufferPool
  ──────────────────────────────────────────────────────────
  mmap(MAP_HUGETLB | MAP_HUGE_2MB)
       │
       ▼
  ┌──────────────────────────────────────┐
  │  HugeBufferPool                      │
  │  slot_size=64 KB, count=16           │
  │  Total: 1 MB huge-page backed        │
  │  acquire() → 64 KB buffer (O(1))     │
  │  release() → returns to pool         │
  └──────────────────────────────────────┘

  I/O Slice / prefetch
  ──────────────────────────────────────────────────────────
  IoSlice{base, len}  →  writev() / sendmsg() iovec array
  prefetch_read(ptr)  →  x86 PREFETCHT0 (pulls cache line)
  prefetch_write(ptr) →  x86 PREFETCHW
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `pin_reactor_to_cpu(cpu_id)` | Bind the calling thread to a specific CPU core |
| `HugeBufferPool(slot_size, count)` | Pool of huge-page-backed buffers |
| `HugeBufferPool::acquire()` | Acquire a buffer slot (O(1)) |
| `HugeBufferPool::release(ptr)` | Return slot to pool |
| `IoSlice{ptr, len}` | View over a memory region for scatter-gather I/O |
| `prefetch_read(ptr)` | Software prefetch for read (PREFETCHT0) |
| `prefetch_write(ptr)` | Software prefetch for write (PREFETCHW) |

## Input / Output

| Input | Output |
|-------|--------|
| CPU core index | Thread pinned; cache-miss rate reduced |
| Huge-page pool config | 2 MB-backed buffers; TLB entries reduced |
| IoSlice array | Compatible `iovec*` for `writev(2)` |

## How to Run

```bash
# Build (Linux only; requires kernel huge-page support)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target numa_hugepages_example

# Run (may require /proc/sys/vm/nr_hugepages > 0)
./build/examples/03-memory/numa_hugepages/numa_hugepages_example
```

## Expected Output

```
[numa] pinned thread to cpu=0
[huge] acquired buffer at 0x... (64 KB)
[huge] released buffer
[ioslice] 3 slices, total=... bytes
[prefetch] prefetch_read/write OK
```

## Notes

- Huge-page allocation falls back to regular `mmap` if the kernel has no free huge pages. Check `cat /proc/sys/vm/nr_hugepages`.
- `pin_reactor_to_cpu` uses `pthread_setaffinity_np` on Linux; it is a no-op on macOS.
- IoSlices can be passed directly to `writev(2)` or `sendmsg(2)` for scatter-gather network I/O.
