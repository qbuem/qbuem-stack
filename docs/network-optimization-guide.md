# qbuem-stack: Network Performance Optimization Guide

This guide outlines the "Extreme Performance" implementation strategies for the `net` module, targeting the world-record class 40M RPS and 200ns latency benchmarks.

## 1. TCP: The io_uring Mastery

### 1.1 Fixed Buffers (Registered Buffers)
To achieve zero-copy, memory regions must be pre-registered with `io_uring_register()`.
- **Strategy**: implement `FixedBufferPool` in `qbuem::buf`. Use these buffers for all `TcpStream` operations.
- **Benefit**: Eliminates page pinning and `memcpy` during kernel-user transitions.

### 1.2 Multishot Operations
- **Multishot Accept**: A single submission for the entire server lifecycle.
- **Multishot Receive**: Use `IORING_OP_RECV_MULTISHOT` with a buffer ring.
- **Benefit**: Reduces SQE submissions by 90% in high-concurrency scenarios.

### 1.3 Busy Polling (`SO_BUSY_POLL`)
- **Strategy**: Enable socket-level busy polling for ultra-low latency reactors.
- **Caution**: Only recommended for NUMA-pinned, dedicated reactor cores.

---

## 2. UDP: Kernel Bypass & Batching

### 2.1 GSO/GRO (Generic Segmentation/Receive Offload)
Instead of processing individual packets, we process "Monster Packets".
- **GSO**: Send one large buffer (up to 64KB) and let the kernel/NIC segment it.
- **GRO**: Receive aggregated packets as a single large buffer.

### 2.2 AF_XDP (Address Family eXpress Data Path)
The ultimate goal for v3.0.0.
- **Strategy**: Implement `XdpSocket` using `UMEM` for zero-copy.
- **Zero-copy Mode**: Requires NIC driver support (XDP_ZC).

---

## 3. UDS: Local Zero-Copy

### 3.1 Abstract Namespace
- **Strategy**: Use `\0` prefix for socket paths to bypass filesystem permission/vnode overhead.

### 3.2 memfd_create + SCM_RIGHTS Zero-Copy
For massive inter-process data transfer (e.g., Video streams, DB dumps).
- **Protocol**: 
  1. Sender creates `memfd_create`.
  2. Sender writes data to the memory segment.
  3. Sender sends the FD via UDS `SCM_RIGHTS`.
  4. Receiver `mmap`s the FD.
- **Benefit**: Hardware-level zero-copy without kernel buffering.

---

## 4. Implementation Priorities

| Category | Technique | Milestone |
| :--- | :--- | :--- |
| **TCP** | Fixed Buffers + Multishot | v2.4.0 (Core Refinement) |
| **UDP** | GSO/GRO + MMSG | v2.8.0 (UDP Infrastructure) |
| **UDS** | memfd_create Zero-Copy | v2.6.0 (Advanced Middleware) |
| **Network** | AF_XDP Bypass | v3.0.0 (Ultimate Stack) |
