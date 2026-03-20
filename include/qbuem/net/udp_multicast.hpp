#pragma once

/**
 * @file qbuem/net/udp_multicast.hpp
 * @brief Native UDP multicast — 1:N group-based datagram distribution.
 * @defgroup qbuem_udp_multicast UDP Multicast
 * @ingroup qbuem_net
 *
 * ## Overview
 *
 * UDP multicast delivers a single datagram to all hosts that have joined a
 * multicast group address. It is the standard distribution mechanism for:
 * - **Financial market data feeds** — tick data broadcast to multiple consumers
 * - **Media streaming** — IPTV, live video synchronisation
 * - **Game state sync** — broadcasting world state to all players on a LAN
 * - **Service discovery** — mDNS / SSDP / custom peer discovery
 *
 * ## Addressing
 * - **IPv4 multicast**: addresses 224.0.0.0/4 (224.x.x.x – 239.x.x.x)
 *   - 224.0.0.x: link-local (TTL 1, not forwarded by routers)
 *   - 239.x.x.x: administratively-scoped (organisation-local)
 * - **IPv6 multicast**: addresses ff00::/8
 *   - ff02::1: all-nodes link-local
 *   - ff02::2: all-routers link-local
 *
 * ## Kernel socket options used
 * | Option                | Level          | Purpose                          |
 * |-----------------------|----------------|----------------------------------|
 * | `IP_ADD_MEMBERSHIP`   | `IPPROTO_IP`   | Join an IPv4 multicast group     |
 * | `IP_DROP_MEMBERSHIP`  | `IPPROTO_IP`   | Leave an IPv4 multicast group    |
 * | `IP_MULTICAST_TTL`    | `IPPROTO_IP`   | Set IPv4 multicast hop limit     |
 * | `IP_MULTICAST_LOOP`   | `IPPROTO_IP`   | Enable/disable loopback          |
 * | `IP_MULTICAST_IF`     | `IPPROTO_IP`   | Select outgoing interface        |
 * | `IPV6_JOIN_GROUP`     | `IPPROTO_IPV6` | Join an IPv6 multicast group     |
 * | `IPV6_LEAVE_GROUP`    | `IPPROTO_IPV6` | Leave an IPv6 multicast group    |
 * | `IPV6_MULTICAST_HOPS` | `IPPROTO_IPV6` | Set IPv6 multicast hop limit     |
 * | `IPV6_MULTICAST_LOOP` | `IPPROTO_IPV6` | IPv6 loopback toggle             |
 * | `SO_REUSEPORT`        | `SOL_SOCKET`   | Multiple workers on same port    |
 *
 * ## Usage — publisher
 * @code
 * // Broadcast tick data to all subscribers on 239.1.2.3:5000
 * auto pub = MulticastSocket::create_sender(
 *     SocketAddr::from_ipv4("239.1.2.3", 5000), "eth0");
 * if (!pub) { // error handling }
 *
 * pub->set_ttl(4);
 * pub->set_loopback(false);   // disable local loopback for production
 *
 * Tick tick{...};
 * co_await pub->send(std::as_bytes(std::span{&tick, 1}));
 * @endcode
 *
 * ## Usage — subscriber
 * @code
 * // Receive tick data from the 239.1.2.3 group
 * auto sub = MulticastSocket::create_receiver(
 *     SocketAddr::from_ipv4("239.1.2.3", 5000), "eth0");
 * if (!sub) { // error handling }
 *
 * std::array<std::byte, 1500> buf{};
 * while (!st.stop_requested()) {
 *     auto [n, from] = co_await sub->recv_from(buf, st);
 *     process(std::span{buf.data(), n}, from);
 * }
 * @endcode
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/reactor.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/net/socket_addr.hpp>

#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <net/if.h>
#include <arpa/inet.h>

namespace qbuem {

// ─── MulticastSocket ─────────────────────────────────────────────────────────

/**
 * @brief UDP socket with multicast group membership management.
 *
 * Combines `UdpSocket` functionality with IP multicast socket options.
 * Supports both IPv4 (`IP_ADD_MEMBERSHIP`) and IPv6 (`IPV6_JOIN_GROUP`).
 *
 * Move-only; the socket fd is closed and group memberships are NOT
 * automatically dropped on destruction (the kernel removes memberships
 * automatically when the last fd for the socket is closed).
 */
class MulticastSocket {
public:
    // ── Constructors ─────────────────────────────────────────────────────────

    MulticastSocket() noexcept : fd_(-1) {}
    explicit MulticastSocket(int fd) noexcept : fd_(fd) {}

    ~MulticastSocket() { if (fd_ >= 0) ::close(fd_); }

    MulticastSocket(const MulticastSocket&)            = delete;
    MulticastSocket& operator=(const MulticastSocket&) = delete;

    MulticastSocket(MulticastSocket&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    MulticastSocket& operator=(MulticastSocket&& o) noexcept {
        if (this != &o) { if (fd_ >= 0) ::close(fd_); fd_ = o.fd_; o.fd_ = -1; }
        return *this;
    }

    // ── Factory — sender ─────────────────────────────────────────────────────

    /**
     * @brief Create a multicast sender socket.
     *
     * Creates a UDP socket and binds it to the INADDR_ANY on an OS-assigned
     * port (suitable for sending to a multicast group address).
     *
     * @param group      Multicast group address and port (e.g. 239.1.2.3:5000).
     * @param iface      Network interface name (e.g. "eth0"). Empty = default route.
     * @returns `MulticastSocket` ready to send, or error.
     */
    [[nodiscard]] static Result<MulticastSocket>
    create_sender(SocketAddr group, std::string_view iface = "") noexcept {
        bool ipv6 = (group.family() == SocketAddr::Family::IPv6);
        int domain = ipv6 ? AF_INET6 : AF_INET;
        int fd = ::socket(domain, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) return unexpected(std::error_code(errno, std::system_category()));

        MulticastSocket sock(fd);

        // Bind to INADDR_ANY (sender does not need to join the group)
        if (ipv6) {
            sockaddr_in6 sa{};
            sa.sin6_family = AF_INET6;
            sa.sin6_port   = 0;  // OS assigns port
            if (::bind(fd, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) != 0) {
                return unexpected(std::error_code(errno, std::system_category()));
            }
            // Set outgoing interface by index
            if (!iface.empty()) {
                unsigned idx = ::if_nametoindex(std::string(iface).c_str());
                if (::setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF,
                                 &idx, sizeof(idx)) != 0) {
                    return unexpected(std::error_code(errno, std::system_category()));
                }
            }
        } else {
            sockaddr_in sa{};
            sa.sin_family = AF_INET;
            sa.sin_port   = 0;
            if (::bind(fd, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) != 0) {
                return unexpected(std::error_code(errno, std::system_category()));
            }
            // Set outgoing interface by address
            if (!iface.empty()) {
                in_addr ifaddr{};
                // Try to resolve interface name → address via SIOCGIFADDR
                // Fallback: use INADDR_ANY (kernel selects interface)
                ::inet_pton(AF_INET, std::string(iface).c_str(), &ifaddr);
                ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF,
                             &ifaddr, sizeof(ifaddr));
            }
        }

        sock.group_ = group;
        return sock;
    }

    /**
     * @brief Create a multicast receiver socket bound to the group port.
     *
     * Binds to `INADDR_ANY:port` (or `[::]:port` for IPv6), joins the
     * specified multicast group, and enables `SO_REUSEPORT` so multiple
     * workers can receive on the same port.
     *
     * @param group  Multicast group address and port.
     * @param iface  Network interface name (e.g. "eth0"). Empty = all interfaces.
     * @returns `MulticastSocket` ready to receive, or error.
     */
    [[nodiscard]] static Result<MulticastSocket>
    create_receiver(SocketAddr group, std::string_view iface = "") noexcept {
        bool ipv6 = (group.family() == SocketAddr::Family::IPv6);
        int domain = ipv6 ? AF_INET6 : AF_INET;
        int fd = ::socket(domain, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) return unexpected(std::error_code(errno, std::system_category()));

        // Allow multiple receivers on the same port
        {
            int opt = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        }

        // Bind to INADDR_ANY:port (receive all multicast on this port)
        if (ipv6) {
            sockaddr_in6 sa{};
            sa.sin6_family = AF_INET6;
            sa.sin6_port   = htons(group.port());
            if (::bind(fd, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) != 0) {
                ::close(fd);
                return unexpected(std::error_code(errno, std::system_category()));
            }
        } else {
            sockaddr_in sa{};
            sa.sin_family      = AF_INET;
            sa.sin_port        = htons(group.port());
            sa.sin_addr.s_addr = INADDR_ANY;
            if (::bind(fd, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) != 0) {
                ::close(fd);
                return unexpected(std::error_code(errno, std::system_category()));
            }
        }

        MulticastSocket sock(fd);
        sock.group_ = group;

        // Join the multicast group
        auto jr = sock.join_group_fd(fd, group, iface);
        if (!jr) return unexpected(jr.error());

        return sock;
    }

    // ── Group membership ─────────────────────────────────────────────────────

    /**
     * @brief Join a multicast group on the specified interface.
     *
     * A single socket can join multiple groups simultaneously.
     *
     * @param group  Multicast group address (port is ignored for membership).
     * @param iface  Interface name (empty = INADDR_ANY / default interface).
     * @returns `Result<void>` — ok on success.
     */
    [[nodiscard]] Result<void>
    join_group(SocketAddr group, std::string_view iface = "") noexcept {
        return join_group_fd(fd_, group, iface);
    }

    /**
     * @brief Leave a previously joined multicast group.
     *
     * @param group  The same group address passed to `join_group()`.
     * @param iface  The same interface passed to `join_group()`.
     * @returns `Result<void>` — ok on success.
     */
    [[nodiscard]] Result<void>
    leave_group(SocketAddr group, std::string_view iface = "") noexcept {
        if (group.family() == SocketAddr::Family::IPv6) {
            ipv6_mreq mreq{};
            mreq.ipv6mr_multiaddr = group.addr_.ipv6_;
            mreq.ipv6mr_interface = iface.empty()
                ? 0 : ::if_nametoindex(std::string(iface).c_str());
            if (::setsockopt(fd_, IPPROTO_IPV6, IPV6_LEAVE_GROUP,
                             &mreq, sizeof(mreq)) != 0)
                return unexpected(std::error_code(errno, std::system_category()));
        } else {
            ip_mreq mreq{};
            mreq.imr_multiaddr = group.addr_.ipv4_;
            if (iface.empty()) {
                mreq.imr_interface.s_addr = INADDR_ANY;
            } else {
                ::inet_pton(AF_INET, std::string(iface).c_str(), &mreq.imr_interface);
            }
            if (::setsockopt(fd_, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                             &mreq, sizeof(mreq)) != 0)
                return unexpected(std::error_code(errno, std::system_category()));
        }
        return {};
    }

    // ── Multicast socket options ──────────────────────────────────────────────

    /**
     * @brief Set the IPv4 multicast TTL (hop limit) for outgoing datagrams.
     *
     * @param ttl  Hop limit (1 = link-local, 32 = site-local, 255 = global).
     * @returns `Result<void>`.
     */
    [[nodiscard]] Result<void> set_ttl(int ttl) noexcept {
        if (group_.family() == SocketAddr::Family::IPv6) {
            if (::setsockopt(fd_, IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
                             &ttl, sizeof(ttl)) != 0)
                return unexpected(std::error_code(errno, std::system_category()));
        } else {
            unsigned char uc = static_cast<unsigned char>(ttl);
            if (::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL,
                             &uc, sizeof(uc)) != 0)
                return unexpected(std::error_code(errno, std::system_category()));
        }
        return {};
    }

    /**
     * @brief Enable or disable multicast loopback.
     *
     * When enabled (default), multicast datagrams sent by this socket are
     * looped back to other sockets on the same host that have joined the group.
     * Disable for production deployments where self-reception is unwanted.
     *
     * @param enabled  True to enable loopback (default), false to disable.
     * @returns `Result<void>`.
     */
    [[nodiscard]] Result<void> set_loopback(bool enabled) noexcept {
        if (group_.family() == SocketAddr::Family::IPv6) {
            unsigned int v = enabled ? 1u : 0u;
            if (::setsockopt(fd_, IPPROTO_IPV6, IPV6_MULTICAST_LOOP,
                             &v, sizeof(v)) != 0)
                return unexpected(std::error_code(errno, std::system_category()));
        } else {
            unsigned char v = enabled ? 1u : 0u;
            if (::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP,
                             &v, sizeof(v)) != 0)
                return unexpected(std::error_code(errno, std::system_category()));
        }
        return {};
    }

    // ── Asynchronous I/O ─────────────────────────────────────────────────────

    /**
     * @brief Asynchronously send a datagram to the multicast group address.
     *
     * Suspends on the reactor until the socket becomes writable.
     *
     * @param buf  Datagram payload.
     * @returns Bytes sent, or error.
     */
    [[nodiscard]] Task<Result<size_t>>
    send(std::span<const std::byte> buf) {
        sockaddr_storage ss{};
        socklen_t sslen{};
        auto r = group_.to_sockaddr(ss, sslen);
        if (!r) co_return unexpected(r.error());

        struct SendAwaiter {
            int fd_;
            const std::byte* data_;
            size_t           size_;
            const sockaddr_storage* ss_;
            socklen_t        sslen_;
            ssize_t result_{-1};
            int     err_{0};

            [[nodiscard]] bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                auto* rx = Reactor::current();
                if (rx == nullptr) { h.resume(); return; }
                rx->register_event(fd_, EventType::Write, [h, this](int f) {
                    result_ = ::sendto(f, data_, size_, 0,
                                       reinterpret_cast<const sockaddr*>(ss_), sslen_);
                    err_ = (result_ < 0) ? errno : 0;
                    Reactor::current()->unregister_event(f, EventType::Write);
                    h.resume();
                });
            }
            void await_resume() const noexcept {}
        };

        SendAwaiter aw{fd_, buf.data(), buf.size(), &ss, sslen};
        co_await aw;
        if (aw.result_ < 0)
            co_return unexpected(std::error_code(aw.err_, std::system_category()));
        co_return static_cast<size_t>(aw.result_);
    }

    /**
     * @brief Asynchronously receive a multicast datagram.
     *
     * @param buf  Receive buffer.
     * @param st   Cancellation token.
     * @returns Pair of (bytes received, sender address), or error.
     */
    [[nodiscard]] Task<Result<std::pair<size_t, SocketAddr>>>
    recv_from(std::span<std::byte> buf, const std::stop_token& st) {
        if (st.stop_requested())
            co_return unexpected(std::make_error_code(std::errc::operation_canceled));

        struct RecvAwaiter {
            int fd_;
            std::byte* data_;
            size_t size_;
            sockaddr_storage from_{};
            socklen_t fromlen_{sizeof(sockaddr_storage)};
            ssize_t result_{-1};
            int err_{0};

            [[nodiscard]] bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                auto* rx = Reactor::current();
                if (rx == nullptr) { h.resume(); return; }
                rx->register_event(fd_, EventType::Read, [h, this](int f) {
                    result_ = ::recvfrom(f, data_, size_, 0,
                                         reinterpret_cast<sockaddr*>(&from_), &fromlen_);
                    err_ = (result_ < 0) ? errno : 0;
                    Reactor::current()->unregister_event(f, EventType::Read);
                    h.resume();
                });
            }
            void await_resume() const noexcept {}
        };

        RecvAwaiter aw{fd_, buf.data(), buf.size()};
        co_await aw;

        if (aw.result_ < 0)
            co_return unexpected(std::error_code(aw.err_, std::system_category()));

        SocketAddr from;
        if (aw.from_.ss_family == AF_INET) {
            const auto* sa = reinterpret_cast<const sockaddr_in*>(&aw.from_);
            from.family_ = SocketAddr::Family::IPv4;
            from.port_   = ntohs(sa->sin_port);
            from.addr_.ipv4_ = sa->sin_addr;
        } else if (aw.from_.ss_family == AF_INET6) {
            const auto* sa = reinterpret_cast<const sockaddr_in6*>(&aw.from_);
            from.family_ = SocketAddr::Family::IPv6;
            from.port_   = ntohs(sa->sin6_port);
            from.addr_.ipv6_ = sa->sin6_addr;
        }
        co_return std::make_pair(static_cast<size_t>(aw.result_), from);
    }

    /** @brief Returns the underlying file descriptor. */
    [[nodiscard]] int fd() const noexcept { return fd_; }

    /** @brief Returns the multicast group address configured at construction. */
    [[nodiscard]] const SocketAddr& group() const noexcept { return group_; }

private:
    // ── Internal join helper ──────────────────────────────────────────────────

    [[nodiscard]] static Result<void>
    join_group_fd(int fd, SocketAddr group, std::string_view iface) noexcept {
        if (group.family() == SocketAddr::Family::IPv6) {
            ipv6_mreq mreq{};
            mreq.ipv6mr_multiaddr = group.addr_.ipv6_;
            mreq.ipv6mr_interface = iface.empty()
                ? 0 : ::if_nametoindex(std::string(iface).c_str());
            if (::setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP,
                             &mreq, sizeof(mreq)) != 0)
                return unexpected(std::error_code(errno, std::system_category()));
        } else {
            ip_mreq mreq{};
            mreq.imr_multiaddr = group.addr_.ipv4_;
            if (iface.empty()) {
                mreq.imr_interface.s_addr = INADDR_ANY;
            } else {
                ::inet_pton(AF_INET, std::string(iface).c_str(), &mreq.imr_interface);
            }
            if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                             &mreq, sizeof(mreq)) != 0)
                return unexpected(std::error_code(errno, std::system_category()));
        }
        return {};
    }

    int       fd_;     ///< UDP socket file descriptor
    SocketAddr group_; ///< Multicast group address and port
};

} // namespace qbuem

/** @} */ // end of qbuem_udp_multicast
