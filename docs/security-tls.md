# Architecture: Zero-dependency Security & TLS

This document specifies the security architecture for `qbuem-stack`, focusing on **Zero-dependency TLS**, Memory Safety, and Hardware-accelerated Cryptography.

---

## 1. Security Philosophy

`qbuem-stack` aims for "Defense in Depth" without relying on bloated external libraries. We prioritize kernel-native security features and audited, minimized cryptographic implementations.

---

## 2. Zero-dependency TLS (The TLS-uring Strategy)

Implementing a full TLS stack from scratch is non-trivial. `qbuem-stack` uses an **Abstraction-over-Implementation** approach.

### 2.1 KTLS (Kernel TLS) Integration
The primary strategy is to leverage Linux **KTLS** (`AF_ALG` and `SO_TLS`).
- **Mechanism**: The stack performs the TLS Handshake (using a minimized provider) and then moves the symmetric encryption keys to the kernel.
- **Benefit**: `io_uring` can perform `READ/WRITE` directly on encrypted sockets, achieving near-line-rate performance with zero user-space encryption overhead.

### 2.2 Provider Interface (`ISecurityProvider`)
A thin abstraction layer allows plugging in different providers:
- **`DefaultProvider`**: Minimalist TLS 1.3 implementation (Header-only).
- **`BoringProvider`**: For users who require FIPS or specific compliance.

---

## 3. High-Performance Security Pillars

### 3.1 Zero-copy Authentication (Token-uring)
- **Problem**: Parsing JWT/Bearer tokens on every request is expensive.
- **Solution**: A specialized `Action` that validates tokens using SIMD instructions.
- **Design**: The token signature is verified against a public key cached in the `IReactor` local memory.

### 3.2 Hardware Entropy Injection
- Uses `RDRAND` / `RDSEED` (Intel/AMD) directly via inline assembly to ensure high-quality random number generation without entropy exhaustion.

### 3.3 Memory Hardening
- **Buffer Scrubbing**: Automatic zeroing of buffers containing sensitive data (headers, tokens) upon return to the `BufferPool`.
- **Isolation**: Important security tasks (like Key Management) are encouraged to run in separate processes connected via **SHM Messaging**, isolated via Linux namespaces.

---

## 4. Security TODO List (Roadmap)

### 4.1 Phase 1: Transport Security
- [ ] `io_uring` support for `sendmsg/recvmsg` with KTLS.
- [ ] TLS 1.3 Handshake state machine (RFC 8446).

### 4.2 Phase 2: Application Security
- [ ] SIMD JWT Parser (Zero-allocation).
- [ ] Rate Limiting Action (Leaky Bucket over `atomic`).
- [ ] WAF (Web Application Firewall) Lite Action for SQLi/XSS filtering.

---

## 5. Conclusion

By integrating security directly into the `IReactor` and leveraging Kernel-level features, `qbuem-stack` provides a secure-by-default environment that is faster than traditional "Secure-Proxy" (Nginx/Envoy) setups.
