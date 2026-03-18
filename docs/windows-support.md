# Windows Support & "Elite" RIO Integration

> **Target Version**: v1.8.0-draft
> **Status**: High-Performance Specification

---

## 1. The Windows Performance Tiers

To reach the theoretical limits of Windows, `qbuem-stack` implements two reactor tiers:

### Tier 1: IOCP (Standard Scalability)
- **Mechanism**: `GetQueuedCompletionStatusEx`.
- **Context**: Standard Proactor model.
- **Benefit**: High connection counts with minimal thread overhead.

### Tier 2: RIO (Extreme Performance - Registered I/O)
- **Mechanism**: `RIOSend`, `RIORecieve`, and `RIODequeueCompletion`.
- **Strategy**: 
    - **Pre-registered Buffers**: Memory is locked and registered with the kernel once (via `RIORegisterBuffer`).
    - **User-mode Completion**: Completions can be polled directly in user-space without system calls (similar to `io_uring` CQ polling).
- **Benefit**: 
    - 30-50% higher throughput for small packets.
    - Zero context-switch overhead in the hot path.
    - Perfect alignment with `qbuem::Arena` and `BufferPool`.

---

## 2. Technical Architecture: "The RIO Bridge"

### 2.1 Buffer Management
RIO requires buffers to be part of a `RIO_BUFFERID`. 
- **qbuem Integration**: Our `FixedPoolResource` will be modified to register its entire memory block with RIO at startup.
- **Zero-Copy**: Sockets directly DMA into the registered pool.

### 2.2 RIOCP (RIO + IOCP)
- For maximum versatility, RIO completion queues can be associated with an IOCP.
- This allows the `WinReactor` to handle both standard handles (files, pipes) and RIO-optimized sockets in a single event loop.

---

## 3. Implementation Roadmap (Revised)

| Feature | Technique | Milestone |
| :--- | :--- | :--- |
| **Foundation** | Unified `Fd` / `SOCKET` abstraction | v1.8.0 |
| **I/O Core** | IOCP Proactor Loop | v1.8.0 |
| **Elite Path** | Windows RIO (Registered I/O) | v1.9.0 |
| **Zero-Copy** | `TransmitFile` (Windows `sendfile`) | v1.8.5 |
| **IPC** | Named Pipes (overlapped) | v1.8.5 |

---

*qbuem-stack — Reaching the absolute limits of the Windows NT kernel.*
