# Zero-dependency Security & "Elite" TLS

This document specifies the high-performance security architecture, focusing on **Kernel-offloaded TLS** and **SIMD-accelerated Cryptography**.

> **See also**: [`docs/crypto-primitives.md`](crypto-primitives.md) for the complete
> `qbuem::crypto` API reference — SHA-256/512, HMAC, PBKDF2, HKDF, Base64,
> ChaCha20-Poly1305, and AES-GCM primitives.

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

## 3. Intra-Pipeline Crypto Primitives

The `qbuem::crypto` module (see [`crypto-primitives.md`](crypto-primitives.md)) provides
zero-dependency, zero-allocation primitives used throughout the pipeline:

| Primitive | Header | Notes |
| :--- | :--- | :--- |
| SHA-256 / SHA-512 | `crypto/sha256.hpp`, `crypto/sha512.hpp` | SHA-NI / ARM SHA2 acceleration |
| HMAC-SHA-256/512 | `crypto/hmac.hpp` | Constant-time verification |
| PBKDF2 | `crypto/pbkdf2.hpp` | Password hashing (OWASP 2023) |
| HKDF | `crypto/hkdf.hpp` | Session key derivation |
| Base64 / Base64url | `crypto/base64.hpp` | JWT, cookie, URL encoding |
| ChaCha20-Poly1305 | `crypto/chacha20_poly1305.hpp` | AEAD (always safe, no AES-NI) |
| AES-128/256-GCM | `crypto/aes_gcm.hpp` | AEAD (AES-NI / ARM AES only) |
| CSPRNG | `crypto/random.hpp` | getrandom / arc4random / RDRAND |

---

## 4. Implementation Priorities

| Feature | Technique | Priority |
| :--- | :--- | :--- |
| **Crypto Primitives** | `qbuem::crypto` zero-dependency module | ✅ Done |
| **TLS Core** | Linux kTLS Integration | 🔥 High |
| **TLS Hardware**| NIC Hardware Offload (TLS_HW) | ✅ Medium |
| **Intra-Pipe** | AES-NI Intrinsics | ✅ Medium |
| **Identity** | SIMD JWT/Bearer Validation | ✅ Medium |

---

*qbuem-stack — Security at the speed of hardware.*
