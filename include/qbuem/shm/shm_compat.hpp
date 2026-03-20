/**
 * @file shm_compat.hpp
 * @brief POSIX shm_open/shm_unlink compatibility shim for Android Bionic.
 *
 * Android Bionic does not expose shm_open/shm_unlink even on kernels that
 * support the underlying sys_mmap/memfd primitives.  This shim emulates the
 * POSIX interface using an ordinary file under /data/local/tmp (world-readable,
 * world-writable on Android) so that unit tests and benchmarks can compile and
 * run on-device.
 *
 * On all other POSIX platforms the standard <sys/mman.h> declaration is used.
 */
#pragma once

#if defined(__ANDROID__)

#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

namespace qbuem::shm_compat_detail {

inline const char* shm_base_dir() noexcept { return "/data/local/tmp"; }

inline int shm_open_compat(const char* name, int oflag, mode_t mode) noexcept {
    // Build path: /data/local/tmp/<name>  (name always starts with '/')
    char path[256];
    const char* base = shm_base_dir();
    size_t blen = strlen(base);
    size_t nlen = name ? strlen(name) : 0;
    if (blen + nlen + 1 > sizeof(path)) { errno = ENAMETOOLONG; return -1; }
    memcpy(path, base, blen);
    memcpy(path + blen, name, nlen + 1);
    return open(path, oflag | O_CLOEXEC, mode);
}

inline int shm_unlink_compat(const char* name) noexcept {
    char path[256];
    const char* base = shm_base_dir();
    size_t blen = strlen(base);
    size_t nlen = name ? strlen(name) : 0;
    if (blen + nlen + 1 > sizeof(path)) { errno = ENAMETOOLONG; return -1; }
    memcpy(path, base, blen);
    memcpy(path + blen, name, nlen + 1);
    return unlink(path);
}

} // namespace qbuem::shm_compat_detail

// Inject into global scope so call sites don't need changing.
inline int shm_open(const char* name, int oflag, mode_t mode) noexcept {
    return ::qbuem::shm_compat_detail::shm_open_compat(name, oflag, mode);
}
inline int shm_unlink(const char* name) noexcept {
    return ::qbuem::shm_compat_detail::shm_unlink_compat(name);
}

#else
#  include <sys/mman.h>  // POSIX shm_open / shm_unlink
#endif
