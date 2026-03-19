#pragma once

/**
 * @file qbuem/net/dns.hpp
 * @brief Async DNS resolver — non-blocking hostname resolution via thread + Reactor::post().
 * @defgroup qbuem_dns DNS Resolver
 * @ingroup qbuem_net
 *
 * ## Design
 * `getaddrinfo(3)` is a blocking syscall. Calling it on the reactor thread stalls the
 * entire event loop. `DnsResolver::resolve()` offloads the call to a detached thread,
 * then posts the coroutine resume back to the originating reactor via `Reactor::post()`.
 *
 * ## Hostname vs IP literal
 * If the host string is already a numeric IPv4 or IPv6 address, `inet_pton` succeeds
 * and no thread is spawned — resolution completes synchronously in `await_ready()`.
 *
 * ## Thread safety
 * Each call to `resolve()` spawns exactly one short-lived detached thread. The thread
 * writes its result to a shared_ptr-owned slot and then resumes the coroutine through
 * `Reactor::post()`, ensuring the resume happens on the correct reactor thread.
 *
 * ## Usage
 * @code
 * auto addr = co_await DnsResolver::resolve("httpbin.org", 80);
 * if (!addr) co_return unexpected(addr.error());
 * auto stream = co_await TcpStream::connect(*addr);
 * @endcode
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/net/socket_addr.hpp>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <coroutine>
#include <memory>
#include <string>
#include <thread>  // std::jthread

namespace qbuem {

/**
 * @brief Async DNS resolver.
 *
 * Provides a single static coroutine entry point `resolve()` that resolves a
 * hostname to a `SocketAddr` without blocking the reactor event loop.
 */
class DnsResolver {
public:
  /**
   * @brief Resolve a hostname to a `SocketAddr` asynchronously.
   *
   * - If `host` is a numeric IPv4 or IPv6 literal, resolution is immediate (no thread).
   * - Otherwise, `getaddrinfo(3)` runs on a detached thread and the coroutine is
   *   resumed on the calling reactor thread via `Reactor::post()`.
   *
   * @param host  Hostname or IP literal (e.g. "httpbin.org", "127.0.0.1", "::1").
   * @param port  Port number in host byte order.
   * @returns     Resolved `SocketAddr` on success, or an error code on failure.
   *
   * @code
   * auto addr = co_await DnsResolver::resolve("example.com", 80);
   * if (!addr) co_return unexpected(addr.error());
   * @endcode
   */
  [[nodiscard]] static Task<Result<SocketAddr>> resolve(std::string host,
                                                         uint16_t port) {
    co_return co_await Awaiter{std::move(host), port};
  }

private:
  // ── Shared state between coroutine and resolver thread ───────────────────

  struct State {
    Result<SocketAddr> result{
        unexpected(std::make_error_code(std::errc::address_not_available))};
  };

  // ── Coroutine awaiter ─────────────────────────────────────────────────────

  struct Awaiter {
    std::string host;
    uint16_t    port;
    std::shared_ptr<State> state{std::make_shared<State>()};

    // ── Try fast-path: numeric IP literal ────────────────────────────────

    bool await_ready() noexcept {
      // Try IPv4 literal
      {
        auto r = SocketAddr::from_ipv4(host.c_str(), port);
        if (r) { state->result = *r; return true; }
      }
      // Try IPv6 literal
      {
        auto r = SocketAddr::from_ipv6(host.c_str(), port);
        if (r) { state->result = *r; return true; }
      }
      return false; // needs async resolution
    }

    void await_suspend(std::coroutine_handle<> handle) {
      // Capture reactor pointer before spawning — Reactor::current() is
      // thread-local and only valid on the reactor thread.
      Reactor* reactor = Reactor::current();
      auto     st      = state;

      // Move host into the lambda to avoid an extra string copy.
      // port is a struct member — capture by value via explicit init-capture.
      std::jthread([host = std::move(host),
                   port = port,
                   st,
                   handle,
                   reactor]() mutable {
        // ── getaddrinfo (blocking) ────────────────────────────────────────
        addrinfo hints{};
        hints.ai_family   = AF_UNSPEC;   // IPv4 or IPv6
        hints.ai_socktype = SOCK_STREAM;

        addrinfo* res = nullptr;
        int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &res);
        if (rc != 0 || res == nullptr) {
          st->result = unexpected(
              std::make_error_code(std::errc::address_not_available));
        } else {
          // Prefer IPv4; fall back to IPv6.
          // Use from_sockaddr_in / from_sockaddr_in6 to avoid the
          // inet_ntop → string → inet_pton round-trip.
          SocketAddr found;
          bool       have_addr = false;

          for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
            if (ai->ai_family == AF_INET) {
              auto* sa4 = reinterpret_cast<const sockaddr_in*>(ai->ai_addr);
              found     = SocketAddr::from_sockaddr_in(*sa4, port);
              have_addr = true;
              break;
            }
          }
          if (!have_addr) {
            for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
              if (ai->ai_family == AF_INET6) {
                auto* sa6 = reinterpret_cast<const sockaddr_in6*>(ai->ai_addr);
                found     = SocketAddr::from_sockaddr_in6(*sa6, port);
                have_addr = true;
                break;
              }
            }
          }
          ::freeaddrinfo(res);
          if (have_addr) st->result = found;
        }

        // ── Resume coroutine on the reactor thread ────────────────────────
        if (reactor) {
          reactor->post([handle]() mutable { handle.resume(); });
        } else {
          handle.resume(); // no reactor (e.g. unit test context)
        }
      }).detach();
    }

    Result<SocketAddr> await_resume() {
      return std::move(state->result);
    }
  };
};

} // namespace qbuem

/** @} */ // end of qbuem_dns
