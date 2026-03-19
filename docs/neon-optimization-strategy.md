# ARM NEON SIMD Optimization Strategy

This document identifies files and functions within the `qbuem-stack` project that are candidates for ARM NEON SIMD optimizations.

## 1. High-Priority Candidates (Data Processing)

These components process large buffers and will see the most significant performance gains from vectorization.

| File Path | Function/Logic | Current Status | NEON Potential |
|-----------|----------------|----------------|----------------|
| `include/qbuem/buf/simd_erasure.hpp` | `gf_mul_add` (GF(2^8) Reed-Solomon) | AVX2/SSE4.2 only | Implement split-table lookup using `vtblq_u8` (similar to `vpshufb`). |
| `include/qbuem/security/simd_validator.hpp` | `crc32` (IEEE 802.3) | SSE4.2 only | Use ARMv8-A CRC32 extensions (`__crc32cw`, etc.). |
| `include/qbuem/security/simd_validator.hpp` | `scan_neon` (JSON structural scan) | Basic NEON | Use `vget_lane_u64` and `clz` for faster bitmask processing instead of falling back to scalar early. |
| `include/qbuem/crypto.hpp` | `constant_time_equal` | Scalar loop | Vectorize XOR and OR reduction using 128-bit NEON registers. |
| `include/qbuem/crypto.hpp` | `base64url_encode` | Scalar loop | Implement SIMD base64 encoding using `vtblq_u8` for character translation. |
| `include/qbuem/server/websocket_handler.hpp` | `decode_frame`/`encode_frame` (XOR Masking) | Scalar loop | Vectorize 4-byte mask application across 16-byte chunks. |
| `include/qbuem/server/websocket_handler.hpp` | `compute_accept_key` (SHA-1) | Scalar loop | Use ARMv8-A Cryptography Extensions (`vsha1cq_u32`, etc.). |
| `src/http/parser.cpp` | `find_header_end` (`\r\n\r\n` scan) | Simplified NEON | Improve bitmask extraction to avoid excessive scalar checks on potential hits. |

## 2. Medium-Priority Candidates (Hashing & Searching)

These components are frequently called but usually handle smaller chunks of data per call.

| File Path | Function/Logic | Current Status | NEON Potential |
|-----------|----------------|----------------|----------------|
| `include/qbuem/codec/line_codec.hpp` | `decode` (Line delimiter search) | `std::memchr` | Implement `memchr`-like search for `\r` and `\n` simultaneously using `vceqq_u8` and `vmaxq_u8`. |
| `include/qbuem/security/jwt_action.hpp` | `hash` (FNV-1a) | Scalar loop | Vectorize FNV-1a by processing multiple 32-bit/64-bit words in parallel (if hash collision risks are managed). |
| `include/qbuem/buf/lock_free_hash_map.hpp` | `hash` (Integer mix) | Scalar | Vectorize key mixing for batch insertions. |
| `include/qbuem/db/smart_cache.hpp` | `hash_key` (FNV-1a) | Scalar | Same as `jwt_action`. |
| `include/qbuem/db/simd_parser.hpp` | `parse_row_neon` | In-progress | Ensure full vectorization of column delimiter scanning and NULL bitmap processing. |

## 3. Implementation Guidelines

### Detection
Use the existing `QBUEM_HAS_NEON` or `__ARM_NEON` macros to gate NEON-specific code:

```cpp
#if defined(__ARM_NEON)
#  include <arm_neon.h>
#  define QBUEM_USE_NEON 1
#endif
```

### Common Patterns
1. **XOR/Masking**: Use `veorq_u8` for 128-bit bitwise XOR.
2. **Character Searching**: Use `vceqq_u8` to create a mask and `vmaxvq_u8` (on AArch64) to quickly check if any byte matched.
3. **Table Lookup**: Use `vtbl1_u8` / `vtblq_u8` for fast small-table lookups (useful for Base64 and GF arithmetic).
4. **CRC/SHA**: Prefer ARMv8-A hardware instructions (`__crc32b`, `vsha1mq_u32`) when available, falling back to vectorized software implementations.

## 4. Summary of Targeted Files

### `include/qbuem/`
- `buf/simd_erasure.hpp`
- `buf/lock_free_hash_map.hpp`
- `codec/line_codec.hpp`
- `crypto.hpp`
- `db/simd_parser.hpp`
- `db/smart_cache.hpp`
- `security/simd_validator.hpp`
- `security/jwt_action.hpp`
- `server/websocket_handler.hpp`

### `src/`
- `http/parser.cpp`
- `db/simd_parser.cpp` (Implementation of `simd_parser.hpp`)
