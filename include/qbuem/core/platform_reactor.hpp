#pragma once

/**
 * @file qbuem/core/platform_reactor.hpp
 * @brief Portable alias for the concrete Reactor selected at build time.
 * @ingroup qbuem_core
 *
 * `qbuem::PlatformReactor` resolves to the reactor implementation that the
 * library was actually compiled with, using the SAME selection rule as
 * `Dispatcher` (see `src/core/dispatcher.cpp`):
 *
 *   - macOS / BSD            -> `KqueueReactor`
 *   - Linux with liburing    -> `IOUringReactor`   (when `QBUEM_HAS_IOURING`)
 *   - Linux without liburing -> `EpollReactor`
 *
 * Always prefer this alias over naming a concrete reactor directly. Hardcoding,
 * e.g., `EpollReactor` links fine in a no-liburing build but fails to link in an
 * io_uring build (only `io_uring_reactor.cpp` is compiled then, so
 * `EpollReactor::EpollReactor()` is undefined).
 *
 * `QBUEM_HAS_IOURING` is published as a PUBLIC compile definition on the
 * `qbuem_reactor` target, so any consumer that links the library (directly or
 * through the umbrella) sees the same value the library was built with.
 *
 * ### Usage
 * @code
 * #include <qbuem/core/platform_reactor.hpp>
 *
 * auto reactor = std::make_unique<qbuem::PlatformReactor>();
 * @endcode
 */

#if defined(__APPLE__)
#  include <qbuem/core/kqueue_reactor.hpp>
#elif defined(QBUEM_HAS_IOURING)
#  include <qbuem/core/io_uring_reactor.hpp>
#else
#  include <qbuem/core/epoll_reactor.hpp>
#endif

namespace qbuem {

#if defined(__APPLE__)
/// @brief The concrete Reactor this build uses (see file docs for the rule).
using PlatformReactor = KqueueReactor;
#elif defined(QBUEM_HAS_IOURING)
using PlatformReactor = IOUringReactor;
#else
using PlatformReactor = EpollReactor;
#endif

} // namespace qbuem
