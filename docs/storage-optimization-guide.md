# qbuem-stack: Storage & File I/O Optimization Guide

This guide details "Extreme Performance" strategies for disk I/O, targeting database-grade latency and multi-GB/s throughput.

## 1. The io_uring File Power

### 1.1 O_DIRECT + io_uring
- **Strategy**: Use `O_DIRECT` for all `FileSink`/`FileSource` operations to bypass the Linux Page Cache.
- **Benefit**: Eliminates "double caching" (data in both Page Cache and application buffers) and provides predictable latency by avoiding kernel writeback spikes.
- **Alignment**: Requires 512-byte (or 4KB) alignment for both buffers and offsets.

### 1.2 Registered Buffers (`IORING_REGISTER_BUFFERS`)
- **Strategy**: Pre-register file I/O buffers with the kernel.
- **Benefit**: Removes the overhead of `get_user_pages()` during each read/write call, saving ~5-10% CPU in I/O intensive paths.

---

## 2. Advanced Zero-Copy File Ops

### 2.1 copy_file_range(2)
- **Strategy**: Use for file-to-file transfers (e.g., `cp` style operations).
- **Benefit**: Performs the copy entirely within the kernel. On modern filesystems (XFS, Btrfs), it uses **reflinks** (metadata-only copy) for near-instantaneous duplication of TB-scale files.

### 2.2 sendfile(2) / splice(2)
- **Strategy**: Use `sendfile` to stream files directly to network sockets.
- **Benefit**: Zero-copy from kernel page cache to socket buffers.

---

## 3. Storage Hierarchy v3.0

### 3.1 SPDK (Storage Performance Development Kit)
- **Strategy**: Implement `SpdkBlockDevice` for v3.0.0.
- **Mechanism**: Move NVMe drivers to user-space and use **Polled Mode** (no interrupts).
- **Benefit**: Achieves millions of IOPS per core with sub-10μs latency.

### 3.2 io_uring Fixed Files
- **Strategy**: Use `IORING_REGISTER_FILES` to keep file descriptors open in the kernel.
- **Benefit**: Eliminates the cost of looking up the file in the process's FD table for every I/O.

---

## 4. Storage Implementation Priorities

| Category | Technique | Milestone |
| :--- | :--- | :--- |
| **Direct I/O** | `O_DIRECT` + Fixed Buffers | v2.4.0 (Core Refinement) |
| **Zero-Copy** | `copy_file_range` | v2.6.0 (Advanced WAS) |
| **Filesystem** | Large Page / HugePage Buffers | v2.5.0 (Pipeline+) |
| **Distributed / Fabric** | NVMe-oF (RDMA/TCP) | v3.0.0 (Ultimate Stack) |
| **Resilience** | SIMD Erasure Coding (ISA-L) | v3.0.0 (Ultimate Stack) |
| **Bypass** | SPDK NVMe-oF Initiator | v3.0.0 (Ultimate Stack) |

---

## 5. Distributed Storage & Fabric

### 5.1 NVMe-over-Fabrics (NVMe-oF)
- **Concept**: Accessing remote NVMe drives as if they were local PCIe devices.
- **RDMA Transport (RoCE/InfiniBand)**: Sub-10μs additional latency. Uses `qbuem::transport::rdma`.
- **TCP Transport**: Higher compatibility, uses `qbuem::net::tcp` with `io_uring`.

### 5.2 Client-Side Performance
- **SPDK Initiator**: Total user-space initiator bypassing the kernel's block layer and network stack.
- **Benefit**: 10x lower latency vs kernel initiator.

### 5.3 SIMD Erasure Coding (ISA-L)
- **Strategy**: Use Intel ISA-L (or equivalent SIMD logic) for Reed-Solomon parity.
- **Benefit**: 5x faster than standard libraries. Allows for Cloud-scale data durability with only ~20% storage overhead (instead of 200% for 3x replication).
