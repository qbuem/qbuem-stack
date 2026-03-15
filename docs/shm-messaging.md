# Architecture & Implementation: Zero-copy Shared Memory Messaging

This document provides a highly detailed engineering specification for the `qbuem-stack` Shared Memory (SHM) messaging infrastructure. It is designed to be a definitive guide for both human engineers and AI agents implementing or extending this system.

---

## 1. Architectural Blueprint

The goal is to provide a "Single System Image" feel for messaging, where the physical location of a topic (intra-thread, inter-thread, or inter-process) is abstracted away while maintaining maximum mechanical sympathy.

### 1.1 Messaging Hierarchy

| Scope | Transport Backend | Latency (p99) | Allocation | Sync Mechanism |
| :--- | :--- | :--- | :--- | :--- |
| **Intra-Reactor** | `ArenaChannel<T>` | < 5ns | 0 | Inline Resume |
| **Inter-Thread** | `AsyncChannel<T>` | < 50ns | 0 | `std::atomic` + Reactor Post |
| **Inter-Process** | `SHMChannel<T>` | < 150ns | 0 | `io_uring` Futex (Wait/Wake) |

---

## 2. Shared Memory Infrastructure (The SHMLink)

Each cross-process topic is backed by a dedicated SHM segment created via `memfd_create(2)` or `shm_open(3)`.

### 2.1 Segment Layout (Cache-Line Aligned)

The memory segment is divided into three primary zones. All offsets are relative to the segment base.

```text
[Header (64B)] -> [Metadata Ring (N * 32B)] -> [Data Arena (Variable)]
```

#### 2.1.1 Control Header (Offset 0)
Designed for single-cache-line access to core state.

| Offset | Type | Name | Description |
| :--- | :--- | :--- | :--- |
| 0 | `atomic<u64>` | **Tail** | Global commit index (Producers increment). |
| 8 | `atomic<u64>` | **Head** | Global consumption index (Consumers increment). |
| 16 | `u32` | **Capacity** | Total slots in the Metadata Ring (Must be power of 2). |
| 20 | `u32` | **Magic** | `0x5142554D` ("QBUM") for integrity check. |
| 24 | `atomic<u32>`| **State** | bit 0: Active, bit 1: Draining, bit 2: Error. |
| 64 | - | - | Padding to next cache-line. |

#### 2.1.2 Metadata Slot (32B)
Each slot in the ring buffer contains metadata about the message, not the message itself.

| Field | Size | Name | Description |
| :--- | :--- | :--- | :--- |
| **Sequence**| 8B | `seq` | Vyukov-style sequence for MPMC synchronization. |
| **Offset** | 4B | `off` | Relative offset to the Data Arena. |
| **Length** | 4B | `len` | Length of the payload. |
| **TypeID** | 8B | `tid` | Unique Type ID for schema identification. |
| **Flags** | 4B | `flg` | bit 0: Multipart, bit 1: Compressed, bit 2: Encrypted. |
| **Epoch** | 4B | `epc` | To detect/prevent ABA problems with re-purposed slots. |

---

## 3. Synchronization: The Futex-uring Pattern

To achieve sub-microsecond IPC latency, we eliminate the need for global broker processes for the hot-path.

### 3.1 Producer Workflow
1. **Slot Acquisition**: CAS on `Tail` to reserve a metadata slot.
2. **Data Writing**: Write payload into `Data Arena` (using the `off` calculated from slot index).
3. **Commit**: Update slot `sequence` to current `tail + 1`.
4. **Signal**: 
   - If `Head == Tail-1` (buffer was empty), invoke `IORING_OP_FUTEX_WAKE` on the `Tail` address.
   - This notifies the kernel to wake up any `IORING_OP_FUTEX_WAIT`ing consumers.

### 3.2 Consumer Workflow (Async/Coroutine)
1. **Poll**: Attempt `try_recv()` from the Metadata Ring.
2. **Suspend**: If empty, issue `IORING_OP_FUTEX_WAIT` via `io_uring` SQE.
3. **Resume**: The kernel resumes the reactor thread when the producer signals.
4. **Read**: Read directly from the `Data Arena` shared address. **No copy into user-space buffer.**

---

## 4. Unified Message Bus (The Transport Layer)

The `MessageBus` acts as a facade. It uses a **Radix Tree** to store topic descriptors.

### 4.1 Topic Descriptor

```cpp
struct TopicDescriptor {
    std::string name;
    TopicScope  scope; // LOCAL_ONLY, SYSTEM_WIDE
    void*       shm_base; // nullptr for local
    // ... metadata
};
```

### 4.2 Cross-Process Type Safety

When a message is received from SHM, the `TypeID` is used to identify the data structure.
- The `qbuem-stack` provides the infrastructure for transporting any type-erased data.
- Validation and decoding are handled by upper layers or specific plugins, keeping the core stack **zero-dependency**.

---

## 5. Distributed Pipeline Integration

Standard pipeline stages (`Action<In, Out>`) are extended with `SHM` support.

### 5.1 SHMSource / SHMSink

```cpp
// Explicit Pipeline Head
auto pipeline = PipelineBuilder<MyMsg>()
    .source<SHMSource>("system.raw_ingress") // IPC Ingress
    .add<Processed>(logic_action)
    .sink<SHMSink>("system.analytics")       // IPC Egress
    .build();
```

### 5.2 Context Propagation (The 128B Envelope)

Tracing and Auth data are packed into a fixed-width envelope preceding the payload in the Data Arena.

```cpp
struct SHMEnvelope {
    u128 trace_id;   // W3C Trace
    u64  span_id;
    u64  type_id;    // Unique structure identifier
    u8   auth_token[32]; 
    u8   reserved[64]; // Padding for 128B alignment
};
```

---

## 6. Failure Modes & Reliability

- **Process Crash**: If a producer crashes mid-write, the `Epoch` and `Sequence` mismatch will eventually be detected by the `IReactor` cleanup task.
- **Buffer Overflow**: `SHMSink` implements strict backpressure by `co_await`ing slot availability.
- **Security**: SHM segments are permissioned via `fchmod(2)`. Only processes in the same group can map the messaging segments.

---

## 7. Performance Targets

- **Throughput**: 5M+ messages/sec (small payloads).
- **Latency**: < 100ns (Producer commit to Consumer wake).
- **Footprint**: 0 heap allocations during hot-path message passing.

---
