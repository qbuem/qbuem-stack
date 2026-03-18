# Zero-dependency Security & "Elite" TLS

This document specifies the high-performance security architecture, focusing on **Kernel-offloaded TLS** and **SIMD-accelerated Cryptography**.

---

## 1. Transport Security (TLS)

### 1.1 kTLS (Kernel TLS) & TLS_HW Offload
- **Strategy**: Leverage Linux `kTLS` for symmetric encryption.
- **Hardware Offload (`TLS_HW`)**:
    - Use NICs (Mellanox, Chelsio) that support inline encryption/decryption.
    - **Mechanism**: The NIC handles AES-GCM entirely. The CPU only participates in the handshake.
    - **Benefit**: Sub-microsecond secure latency and 100GbE line-rate performance with < 10% CPU usage.

### 1.2 Zero-Allocation Handshake
- **Strategy**: Use a minimalist TLS 1.3 state machine that uses `qbuem::Arena` for temporary handshake buffers.
- **Benefit**: Zero heap fragmentation during connection establishment.

---

## 2. Intra-Pipeline Cryptography

For internal data protection (e.g., Shm communication between processes).

### 2.1 SIMD-AES (AES-NI)
- **Strategy**: Use `_mm_aesenc_si128` (AES-NI) instructions directly via intrinsics.
- **Benefit**: ~0.25 cycles per byte. Intra-pipeline encryption becomes "nearly free".

### 2.2 SIMD-XOR (AVX-512)
- **Strategy**: Masked XOR operations for stream-cipher-style protection.
- **Benefit**: Multi-GB/s data protection for local IPC (SHM) paths.

---

## 3. Implementation Priorities

| Feature | Technique | Priority |
| :--- | :--- | :--- |
| **TLS Core** | Linux kTLS Integration | 🔥 High |
| **TLS Hardware**| NIC Hardware Offload (TLS_HW) | ✅ Medium |
| **Intra-Pipe** | AES-NI Intrinsics | ✅ Medium |
| **Identity** | SIMD JWT/Bearer Validation | ✅ Medium |

---

*qbuem-stack — Security at the speed of hardware.*
