#pragma once

/**
 * @file qbuem/crypto/secure_zero.hpp
 * @brief Optimizer-proof memory wipe for key material.
 * @ingroup qbuem_crypto
 *
 * `secure_zero` overwrites a buffer with zeros in a way the compiler may not
 * elide (a plain `memset` before a free/return is a dead store the optimizer
 * removes). It writes through a `volatile` pointer, whose stores are an
 * observable side effect, so the wipe always happens — used in crypto
 * destructors to clear keys / key-derived state and shrink the window in which
 * secrets sit in freed memory or a core dump.
 *
 * Zero dependency: standard C++ only (no explicit_bzero/memset_s, which are
 * platform-specific).
 */

#include <cstddef>

namespace qbuem::crypto {

/** @brief Overwrite @p n bytes at @p p with zeros; not removable by the optimizer. */
inline void secure_zero(void* p, std::size_t n) noexcept {
    auto* vp = static_cast<volatile unsigned char*>(p);
    while (n-- > 0) *vp++ = 0;
}

/** @brief Convenience: wipe a fixed-size array-like object's bytes. */
template <typename T>
inline void secure_zero(T& obj) noexcept {
    secure_zero(static_cast<void*>(&obj), sizeof(T));
}

} // namespace qbuem::crypto
