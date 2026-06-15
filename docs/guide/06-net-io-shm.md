# Networking, I/O & Shared Memory

This section documents the three lowest-level transport modules in qbuem-stack:

| Namespace / header tree | Role |
|---|---|
| `qbuem` (from `include/qbuem/net/`) | Async sockets: TCP, UDP, Unix domain, DNS, multicast, reliable-UDP, batched UDP, plus the `qbuem::uds` FD-passing helpers. |
| `qbuem` / `qbuem::io` / `qbuem::zero_copy` (from `include/qbuem/io/`) | Zero-copy buffer primitives (`IOVec<N>`, `scattered_span`, `IOSlice`, `ReadBuf<N>`, `WriteBuf`, `BufferPool`), async/direct file I/O, and kernel zero-copy transfer (`sendfile`, `splice`, `MSG_ZEROCOPY`). |
| `qbuem::shm` (from `include/qbuem/shm/`) | Cross-process shared-memory IPC: `SHMChannel<T>`, `SHMBus`, futex synchronization primitives, plus the pipeline adapters `SHMSource<T>` / `SHMSink<T>`. |

Everything here obeys the four pillars: **Zero Latency** (reactor-driven, non-blocking syscalls, ≤1 ms waits), **Zero Copy** (`std::span`/`iovec` views, scatter-gather, `sendfile`), **Zero Allocation** (stack-resident value types, `IOVec<N>`, `BufferPool`), and **Zero Dependency** (C++23 stdlib + POSIX syscalls only). Errors are **returned, never thrown**: synchronous APIs return `Result<T>` (`= std::expected<T, std::error_code>`); async APIs return `Task<Result<T>>` (`co_await` then check `if (!r) … r.error()`).

> **Shared conventions across all socket types**
> - All socket/file wrappers are **move-only** RAII types; the fd is `::close()`d in the destructor. A default-constructed object holds `fd_ == -1`.
> - Every fd is created **non-blocking + close-on-exec** via `qbuem::net::make_socket()` (see [socket_compat](#portability-net-socket_compat)).
> - All async I/O suspends on `Reactor::current()`. If no reactor is active on the calling thread (e.g. a unit test), the awaiter resumes synchronously and performs the syscall inline — handy for tests, but you generally drive these from a `Dispatcher`.
> - `Result<void>{}` is the canonical success value for `Result<void>`-returning functions.

---

## 1. Address value type — `SocketAddr`

**What it is.** `qbuem::SocketAddr` (header `qbuem/net/socket_addr.hpp`) is a fixed-size, stack-only value type encoding an IPv4, IPv6, or Unix-domain address in a single struct (a union over `in_addr` / `in6_addr` / `char[108]`). No heap allocation; it converts directly to/from platform `sockaddr` structs and renders to a string in a caller-supplied buffer.

**When to use it.** Always — it is the address currency for every socket factory (`TcpStream::connect`, `TcpListener::bind`, `UdpSocket::bind`, `MulticastSocket::*`, `RudpSocket::*`, `UdpMmsgSocket::bind`). For hostnames (not literals), resolve first with [`DnsResolver`](#9-async-dns-resolution--dnsresolver).

**API.**

| Member | Signature | Notes |
|---|---|---|
| `from_ipv4` | `static Result<SocketAddr> from_ipv4(const char* ip, uint16_t port) noexcept` | `inet_pton(AF_INET)`. Port in host byte order. |
| `from_ipv6` | `static Result<SocketAddr> from_ipv6(const char* ip, uint16_t port) noexcept` | `inet_pton(AF_INET6)`. |
| `from_unix` | `static Result<SocketAddr> from_unix(const char* path) noexcept` | Path ≤ 107 bytes else `errc::invalid_argument`. |
| `from_sockaddr_in` | `static SocketAddr from_sockaddr_in(const sockaddr_in&, uint16_t port) noexcept` | Binary, no string round-trip (used by DNS). |
| `from_sockaddr_in6` | `static SocketAddr from_sockaddr_in6(const sockaddr_in6&, uint16_t port) noexcept` | |
| `to_sockaddr` | `Result<void> to_sockaddr(sockaddr_storage& out, socklen_t& len) const noexcept` | Fill for `connect`/`bind`/`sendto`. |
| `to_chars` | `int to_chars(char* buf, size_t n) const noexcept` | Zero-alloc render: `127.0.0.1:8080` / `[::1]:8080` / `unix:/tmp/x.sock`. Returns chars written or `-1`. |
| `family()` / `port()` | `Family family() const noexcept` / `uint16_t port() const noexcept` | `Family` ∈ `{IPv4, IPv6, Unix}`. |

```cpp
#include <qbuem/net/socket_addr.hpp>
using namespace qbuem;

auto a = SocketAddr::from_ipv4("127.0.0.1", 8080);
if (!a) { /* a.error() == errc::invalid_argument */ }

char buf[64];
int n = a->to_chars(buf, sizeof(buf));   // n>0; buf == "127.0.0.1:8080"
```

**Gotchas.** `to_chars` needs at least `INET6_ADDRSTRLEN + 10` bytes for IPv6. `port()` is always `0` for Unix-domain addresses. The struct is trivially small — pass it **by value** (the factories take `SocketAddr` by value deliberately).

---

## 2. TCP — `TcpListener` and `TcpStream`

Headers: `qbuem/net/tcp_listener.hpp`, `qbuem/net/tcp_stream.hpp`. Runnable example: `examples/02-network/tcp_echo_server/`.

### 2.1 `TcpListener`

**What it is.** An async listening socket. `bind()` is synchronous; `accept()` is a coroutine returning a connected `TcpStream`.

**When to use it.** Building a TCP server. For higher-level HTTP, prefer the `http`/`server` layer which composes `TcpListener` for you. Use raw `TcpListener` for custom binary protocols or echo/relay servers.

**API.**

| Member | Signature |
|---|---|
| `bind` | `static Result<TcpListener> bind(SocketAddr addr) noexcept` |
| `accept` | `Task<Result<TcpStream>> accept()` |
| `fd` | `int fd() const noexcept` |

`bind()` applies, automatically and best-effort: `SO_REUSEPORT` (multiple workers share one port), `SO_REUSEADDR` (reuse `TIME_WAIT`), and where the platform defines them, `TCP_FASTOPEN` (queue 128) and `TCP_DEFER_ACCEPT` (Linux). It calls `listen(fd, SOMAXCONN)`. `accept()` produces a client fd that is already non-blocking + close-on-exec (`accept4` on Linux, `accept` + `fcntl` on macOS).

### 2.2 `TcpStream`

**What it is.** A connected async TCP socket with read/write and scatter-gather `readv`/`writev`.

**API.**

| Member | Signature | Returns |
|---|---|---|
| `connect` | `static Task<Result<TcpStream>> connect(SocketAddr addr)` | Connected stream. Non-blocking `connect` + reactor wait on `EINPROGRESS`, checks `SO_ERROR`. |
| `read` | `Task<Result<size_t>> read(std::span<std::byte> buf)` | Bytes read; **0 == EOF**. |
| `write` | `Task<Result<size_t>> write(std::span<const std::byte> buf)` | Bytes written (may be short). |
| `readv` | `Task<Result<size_t>> readv(std::span<iovec> bufs)` | Scatter read, total bytes; 0 == EOF. |
| `writev` | `Task<Result<size_t>> writev(std::span<const iovec> bufs)` | Gather write, total bytes. A `scattered_span` converts implicitly to the argument. |
| `set_nodelay` | `void set_nodelay(bool) const noexcept` | `TCP_NODELAY` (disable Nagle — low latency). |
| `set_keepalive` | `void set_keepalive(bool) const noexcept` | `SO_KEEPALIVE`. |
| `fd` | `int fd() const noexcept` | |

**Server skeleton** (mirrors `examples/02-network/tcp_echo_server/`):

```cpp
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/net/tcp_listener.hpp>
#include <qbuem/net/tcp_stream.hpp>
using namespace qbuem;

Task<Result<void>> handle(TcpStream s) {
    std::array<std::byte, 1024> buf{};
    for (;;) {
        auto n = co_await s.read(buf);
        if (!n || *n == 0) break;                 // error or peer EOF
        std::span<const std::byte> view{buf.data(), *n};
        if (auto w = co_await s.write(view); !w) break;
    }
    co_return {};
}

Task<Result<void>> accept_loop(TcpListener& l, Dispatcher& d) {
    for (;;) {
        auto cli = co_await l.accept();
        if (!cli) break;                          // listener closed
        cli->set_nodelay(true);
        d.spawn(handle(std::move(*cli)));         // one coroutine per conn
    }
    co_return {};
}

int main() {
    auto addr = SocketAddr::from_ipv4("0.0.0.0", 8080);
    auto lis  = TcpListener::bind(*addr);
    if (!lis) return 1;
    Dispatcher d(1);
    std::jthread th([&]{ d.run(); });
    d.spawn(accept_loop(*lis, d));
    /* ... */
}
```

**Client side:** `auto s = co_await TcpStream::connect(*SocketAddr::from_ipv4("127.0.0.1", 8080)); if (!s) { ... }`.

**Gotchas.**
- `read`/`write` return **bytes actually transferred** — TCP is a stream, so loop until you have what you need; a partial `write` is normal under backpressure.
- `read() == 0` is **EOF, not an error** (`Result` is still "ok"). Treat `!n || *n == 0` as "connection done", as the example does.
- The destructor closes the fd; never `dup`/store the raw `fd()` past the object's lifetime.
- For batched header+body sends, use `writev` with an [`IOVec<N>`/`scattered_span`](#11-scatter-gather-zero-copy-iovecn--scattered_span) instead of two `write` calls — one syscall, no gather copy (this is what `Http1Handler` does internally).

---

## 3. UDP — `UdpSocket`

Header: `qbuem/net/udp_socket.hpp`. Example: `examples/02-network/udp_unix_socket/`.

**What it is.** An async datagram socket. `send_to` targets an explicit destination; `recv_from` returns both the payload length and the sender address.

**When to use it.** Connectionless request/response, discovery, telemetry. For **high packet rates** prefer [`UdpMmsgSocket`](#7-batched-udp--udpmmsgsocket) (batched syscalls). For **group fan-out** use [`MulticastSocket`](#6-multicast--multicastsocket). For **ordered/reliable** delivery over UDP use [`RudpSocket`](#8-reliable-udp--rudpsocket).

**API.**

| Member | Signature |
|---|---|
| `bind` | `static Result<UdpSocket> bind(SocketAddr addr) noexcept` (enables `SO_REUSEPORT`) |
| `send_to` | `Task<Result<size_t>> send_to(std::span<const std::byte> buf, SocketAddr dest)` |
| `recv_from` | `Task<Result<std::pair<size_t, SocketAddr>>> recv_from(std::span<std::byte> buf)` |
| `fd` | `int fd() const noexcept` |

```cpp
#include <qbuem/net/udp_socket.hpp>
using namespace qbuem;

// receiver
auto sock = UdpSocket::bind(*SocketAddr::from_ipv4("0.0.0.0", 9000));
std::array<std::byte, 1500> buf{};
auto r = co_await sock->recv_from(buf);
if (r) { auto [n, from] = *r; /* span{buf.data(), n}, from.port() */ }

// sender (bind to ephemeral port 0)
auto tx = UdpSocket::bind(*SocketAddr::from_ipv4("127.0.0.1", 0));
std::string_view msg = "hello";
co_await tx->send_to(std::as_bytes(std::span{msg.data(), msg.size()}), *SocketAddr::from_ipv4("127.0.0.1", 9000));
```

**Gotchas.** Size `buf` for your MTU (1500 is typical; jumbo frames differ). A datagram larger than the buffer is **truncated**. `recv_from` only decodes IPv4/IPv6 sender addresses; for other families the returned `SocketAddr` is default (IPv4, port 0).

---

## 4. Unix domain sockets — `UnixSocket`

Header: `qbuem/net/unix_socket.hpp`. Example: `examples/02-network/udp_unix_socket/` (§2).

**What it is.** An async `AF_UNIX`, `SOCK_STREAM` socket for local IPC: lower overhead than loopback TCP and the foundation for FD passing (see [§5](#5-uds-fd-passing--qbuemuds)).

**When to use it.** Same-host process-to-process streams (sidecar, control plane, worker handoff). When you also need to transfer file descriptors or verify the peer's UID/PID, layer the `qbuem::uds` free functions on top of a `UnixSocket::fd()`. For zero-copy *data* IPC at ~150 ns, use [`SHMChannel`](#13-shmchannelt--cross-process-zero-copy-channel) instead.

**API.**

| Member | Signature |
|---|---|
| `bind` | `static Result<UnixSocket> bind(const char* path) noexcept` (unlinks stale file, then `bind`+`listen`) |
| `connect` | `static Task<Result<UnixSocket>> connect(const char* path)` |
| `accept` | `Task<Result<UnixSocket>> accept()` |
| `read` | `Task<Result<size_t>> read(std::span<std::byte> buf)` (0 == EOF) |
| `write` | `Task<Result<size_t>> write(std::span<const std::byte> buf)` |
| `fd` | `int fd() const noexcept` |

```cpp
#include <qbuem/net/unix_socket.hpp>
using namespace qbuem;

// server
auto srv = UnixSocket::bind("/tmp/app.sock");
auto cli = co_await srv->accept();
std::array<std::byte, 128> buf{};
auto n = co_await cli->read(buf);
if (n && *n) co_await cli->write(std::span(buf.data(), *n));   // echo

// client
auto conn = co_await UnixSocket::connect("/tmp/app.sock");
co_await conn->write(std::as_bytes(std::span{"hi", 2}));
```

**Gotchas.** `bind()` unlinks any existing socket file first; the file is **not** removed on destruction — `::unlink(path)` yourself on shutdown. Path length cap is `sizeof(sockaddr_un::sun_path)` (typically 108). `connect()` is async (handles `EINPROGRESS` via the reactor); `bind()`/`accept()`'s listen socket must be driven from a reactor.

---

## 5. UDS FD passing & credentials — `qbuem::uds`

Header: `qbuem/net/uds_advanced.hpp`. Example: `examples/06-ipc-messaging/scatter_send/` (§6). **Linux + macOS only** (other platforms return `errc::not_supported`).

**What it is.** Free functions over a connected UDS fd for: passing file descriptors across processes (`SCM_RIGHTS`), sending FDs together with a zero-copy multi-segment payload (`sendmsg` + `iovec`), and querying peer credentials (`SO_PEERCRED` / `LOCAL_PEERCRED`). Plus Linux **abstract namespace** bind/connect helpers (no filesystem entry).

**When to use it.** A service manager handing a listening socket or a `memfd` SHM segment to a worker; TLS/connection handover between processes; authenticating a local peer before trusting a request. Not for bulk data — that's [`SHMChannel`](#13-shmchannelt--cross-process-zero-copy-channel).

**API.**

| Function | Signature |
|---|---|
| `send_fds` | `Result<ssize_t> send_fds(int sockfd, std::span<const int> fds, std::span<const uint8_t> data = {}) noexcept` |
| `send_fds` (scatter) | `Result<ssize_t> send_fds(int sockfd, std::span<const int> fds, scattered_span data) noexcept` |
| `recv_fds` | `Result<RecvFdsResult> recv_fds(int sockfd, std::span<int> fds_out, std::span<uint8_t> data_buf = {}) noexcept` |
| `get_peer_credentials` | `Result<PeerCredentials> get_peer_credentials(int sockfd) noexcept` |
| `bind_abstract` | `Result<void> bind_abstract(std::string_view name, int type, int& listener) noexcept` (Linux) |
| `connect_abstract` | `Result<int> connect_abstract(std::string_view name, int type) noexcept` (Linux) |

Types: `struct PeerCredentials { pid_t pid; uid_t uid; gid_t gid; bool is_root(); bool is_uid(uid_t); }`, `struct RecvFdsResult { size_t fd_count; ssize_t data_bytes; }`. Constant: `inline constexpr size_t qbuem::uds::kMaxFdsPerMsg = 253;`.

```cpp
#include <qbuem/net/uds_advanced.hpp>
#include <qbuem/io/iovec.hpp>
#include <qbuem/io/scattered_span.hpp>
using namespace qbuem;

// Sender: pass one fd + a two-segment payload in a single sendmsg syscall.
IOVec<2> vec;
vec.push(header.data(), header.size());
vec.push(body.data(),   body.size());
int shm_fd = /* memfd_create(...) */;
auto sent = uds::send_fds(sock_fd, std::span<const int>{&shm_fd, 1}, scattered_span{vec});
if (!sent) { /* sent.error() */ }

// Receiver: collect the fd(s) + data.
std::array<int, 8> got{};
std::array<uint8_t, 4096> data{};
auto r = uds::recv_fds(sock_fd, got, data);
if (r) { for (size_t i = 0; i < r->fd_count; ++i) { /* use got[i]; close when done */ } }

// Authenticate the peer.
if (auto c = uds::get_peer_credentials(sock_fd); c && c->is_uid(getuid())) { /* trust */ }
```

**Gotchas.**
- Each received FD is a fresh `dup()` in the receiver — **you must `::close()`** every returned fd. `recv_fds` already closes any FDs beyond `fds_out.size()` to prevent leaks.
- `send_fds` requires **at least one fd** (`fds.empty()` → `errc::invalid_argument`) and at most `kMaxFdsPerMsg` (253). For an FD-only send (empty payload), a 1-byte dummy is sent because `SOCK_STREAM` `sendmsg` requires ≥1 data byte.
- The scatter overload passes the `scattered_span`'s `iovec` array directly into `msghdr.msg_iov` — **zero gather copy**, bounded by `IOV_MAX`. The backing `IOVec<N>` must outlive the call.
- macOS `LOCAL_PEERCRED` returns no PID; `PeerCredentials::pid` is `-1` there.

---

## 6. Multicast — `MulticastSocket`

Header: `qbuem/net/udp_multicast.hpp`.

**What it is.** A UDP socket with IPv4/IPv6 group-membership management — one publisher, many subscribers, no per-subscriber state. Sender and receiver are created by distinct factories.

**When to use it.** Market-data fan-out, service discovery, LAN state sync — anywhere a single send must reach N listeners. For point-to-point use plain `UdpSocket`; for WAN/reliability multicast is the wrong tool.

**API (selected).**

| Member | Signature |
|---|---|
| `create_sender` | `static Result<MulticastSocket> create_sender(SocketAddr group, std::string_view iface = "") noexcept` |
| `create_receiver` | `static Result<MulticastSocket> create_receiver(SocketAddr group, std::string_view iface = "") noexcept` |
| `join_group` / `leave_group` | `Result<void> join_group(SocketAddr group, std::string_view iface = "") const noexcept` (and `leave_group`) |
| `set_ttl` | `Result<void> set_ttl(int ttl) noexcept` (multicast hop limit) |
| `set_loopback` | `Result<void> set_loopback(bool enabled) noexcept` |
| `send` | `Task<Result<size_t>> send(std::span<const std::byte> buf)` (to the configured group) |
| `recv_from` | `Task<Result<std::pair<size_t, SocketAddr>>> recv_from(std::span<std::byte> buf, const std::stop_token& st)` |

```cpp
#include <qbuem/net/udp_multicast.hpp>
using namespace qbuem;

// Publisher
auto pub = MulticastSocket::create_sender(*SocketAddr::from_ipv4("239.1.2.3", 5000), "eth0");
pub->set_ttl(4);
pub->set_loopback(false);
Tick t{/*...*/};
co_await pub->send(std::as_bytes(std::span{&t, 1}));

// Subscriber
auto sub = MulticastSocket::create_receiver(*SocketAddr::from_ipv4("239.1.2.3", 5000), "eth0");
std::array<std::byte, 1500> buf{};
while (!st.stop_requested()) {
    auto r = co_await sub->recv_from(buf, st);
    if (r) { auto [n, from] = *r; /* process(span{buf.data(),n}, from) */ }
}
```

**Gotchas.** The receiver enables `SO_REUSEPORT`/`SO_REUSEADDR` and joins the group in `create_receiver`. Group memberships are **not** dropped on destruction — the kernel cleans up when the last fd closes (call `leave_group` explicitly only if you reuse the fd). `set_ttl(1)` keeps traffic on the local subnet (default behavior of many stacks). `recv_from` here takes a `stop_token` for cancellation (unlike `UdpSocket::recv_from`).

---

## 7. Batched UDP — `UdpMmsgSocket`

Header: `qbuem/net/udp_mmsg.hpp`.

**What it is.** A UDP socket that amortizes syscall cost across many datagrams using `recvmmsg`/`sendmmsg` (Linux), with a transparent `recvmsg`/`sendmsg`-loop fallback on macOS/BSD. Receive returns a stack-resident `RecvBatch` whose payloads are exposed zero-copy.

**When to use it.** High packet-rate ingest/egress (tick data, game state, media) where the per-packet syscall is the bottleneck. For low-rate or request/response traffic, plain `UdpSocket` is simpler.

**Types.**
- `RecvBatch<MaxBatch = 64, BufSize = 1500>` — `size_t count`; `std::span<const std::byte> span(i)` (zero-copy); `SocketAddr addr(i)`; `std::vector<std::byte> copy(i)` (only when you must own). All buffers are inline (`alignas(64)`), zero heap.
- `SendBatch<MaxBatch = 64>` — `bool add(std::span<const std::byte> buf, SocketAddr dest)`; `size()`, `empty()`, `clear()`. Stores **references** to caller buffers.

**API.**

| Member | Signature |
|---|---|
| `bind` | `static Result<UdpMmsgSocket> bind(SocketAddr addr) noexcept` (sets `SO_REUSEPORT`, 8 MiB `SO_RCVBUF`) |
| `recv_batch` | `Task<Result<RecvBatch<64,1500>>> recv_batch(const std::stop_token& st)` |
| `send_batch` | `template<size_t N> Task<Result<size_t>> send_batch(const SendBatch<N>& batch, const std::stop_token& st)` |
| `fd` | `int fd() const noexcept` |

```cpp
#include <qbuem/net/udp_mmsg.hpp>
using namespace qbuem;

auto sock = UdpMmsgSocket::bind(*SocketAddr::from_ipv4("0.0.0.0", 9000));

// Receive up to 64 datagrams in one recvmmsg (MSG_WAITFORONE: returns on first arrival).
auto batch = co_await sock->recv_batch(st);
if (batch) {
    for (size_t i = 0; i < batch->count; ++i) {
        auto payload = batch->span(i);   // zero-copy view into inline buffer
        auto from    = batch->addr(i);
        /* process(payload, from) */
    }
}

// Send several datagrams in one sendmmsg.
SendBatch<64> out;
out.add(std::as_bytes(std::span{buf1.data(), buf1.size()}), dest1);
out.add(std::as_bytes(std::span{buf2.data(), buf2.size()}), dest2);
auto sent = co_await sock->send_batch(out, st);   // *sent == datagrams sent
```

**Gotchas.**
- `recv_batch` returns `count == 0` on cancellation or `EAGAIN` (not an error). `recvmmsg` uses `MSG_WAITFORONE` — it returns as soon as one datagram is present, then drains whatever else is already queued (no artificial batching latency).
- `RecvBatch::span(i)` aliases the batch's internal storage — the `RecvBatch` must stay alive while you use the spans. Use `copy(i)` only when handing ownership downstream (e.g. into a channel).
- `SendBatch` holds **non-owning** spans; the source buffers must outlive `send_batch`.
- A 64×1500 `RecvBatch` is ~96 KiB on the stack — fine in a coroutine frame, but don't allocate dozens simultaneously on a tiny stack. Tune `MaxBatch`/`BufSize` via the template parameters if needed.

---

## 8. Reliable UDP — `RudpSocket`

Header: `qbuem/net/rudp_socket.hpp`.

**What it is.** A connection-oriented, ordered, reliable transport layered on `UdpSocket`, with SYN/ACK handshake, sequence numbers, ACK/NACK retransmission, a send window, and graceful FIN close. One `RudpSocket` == one connection.

**When to use it.** You need ordered/reliable delivery but want UDP's lower handshake/latency profile or NAT characteristics, and TCP isn't suitable. If you don't need reliability, use `UdpSocket`/`UdpMmsgSocket`; if you don't need UDP semantics, plain TCP (`TcpStream`) is simpler and battle-tested.

**API (selected).**

| Member | Signature |
|---|---|
| `connect` | `static Task<Result<std::unique_ptr<RudpSocket>>> connect(SocketAddr local, SocketAddr remote, const std::stop_token& st)` |
| `listen` | `static Task<Result<std::unique_ptr<RudpSocket>>> listen(SocketAddr local, const std::stop_token& st)` |
| `send` | `Task<Result<size_t>> send(std::span<const std::byte> data, const std::stop_token& st)` |
| `recv` | `Task<Result<size_t>> recv(std::span<std::byte> out, const std::stop_token& st)` |
| `close` | `Task<void> close(const std::stop_token& st)` (sends FIN) |
| `send_seq()` / `recv_seq()` | `uint32_t … const noexcept` |

```cpp
#include <qbuem/net/rudp_socket.hpp>
using namespace qbuem;

// Server: accept the first incoming connection.
auto srv = co_await RudpSocket::listen(*SocketAddr::from_ipv4("0.0.0.0", 7000), st);
std::array<std::byte, 2048> buf{};
auto n = co_await (*srv)->recv(buf, st);

// Client: connect and send.
auto cli = co_await RudpSocket::connect(
    *SocketAddr::from_ipv4("0.0.0.0", 0),
    *SocketAddr::from_ipv4("127.0.0.1", 7000), st);
co_await (*cli)->send(std::as_bytes(std::span{"hello RUDP", 10}), st);
co_await (*cli)->close(st);
```

**Gotchas.** Returned as a heap `std::unique_ptr` (the connection holds an out-of-order receive buffer and retransmit state, so it is not a trivial value type — this is one of the few non-trivial allocations in the net layer, justified by per-connection state). `listen()` returns after the **first** peer's handshake — for multiple peers, call it again per connection. Always thread the same `stop_token` through `connect`/`listen`/`send`/`recv`/`close` for clean cancellation.

---

## 9. Async DNS resolution — `DnsResolver`

Header: `qbuem/net/dns.hpp`.

**What it is.** `static Task<Result<SocketAddr>> DnsResolver::resolve(const std::string& host, uint16_t port)`. `getaddrinfo(3)` is blocking, so this offloads it to a short-lived detached `std::jthread` and resumes the coroutine **on the originating reactor** via `Reactor::post()`. Numeric IPv4/IPv6 literals resolve **synchronously** in `await_ready()` (no thread spawned).

**When to use it.** Before any `connect()` to a hostname. Skip it when you already have a literal IP (or call it anyway — the literal fast path is free).

```cpp
#include <qbuem/net/dns.hpp>
#include <qbuem/net/tcp_stream.hpp>
using namespace qbuem;

auto addr = co_await DnsResolver::resolve("example.com", 80);
if (!addr) co_return std::unexpected(addr.error());   // errc::address_not_available
auto stream = co_await TcpStream::connect(*addr);
```

**Gotchas.** Prefers an IPv4 (`A`) result, falling back to IPv6 (`AAAA`); it returns the **first** match, not a full list — no round-robin/failover across addresses. One detached thread per non-literal call: fine for occasional connects, not for resolving thousands of hostnames in a tight loop (cache results yourself). Resume requires a live reactor on the calling thread; without one (e.g. a bare unit test) it resumes inline on the worker thread.

---

## 10. Portability shim — `qbuem::net` (`socket_compat.hpp`) {#portability-net-socket_compat}

Header: `qbuem/net/socket_compat.hpp`. You rarely call these directly (the socket factories do), but they're the seam for porting and for writing your own raw sockets that respect the reactor invariant (non-blocking + close-on-exec).

| Function | Purpose |
|---|---|
| `int net::make_socket(int domain, int type, int protocol) noexcept` | `socket()` that is non-blocking + `CLOEXEC` on Linux (`SOCK_NONBLOCK\|SOCK_CLOEXEC`) and macOS (`fcntl` fallback). Returns fd or `-1` (errno). |
| `int net::make_socket_blocking_cloexec(int, int, int) noexcept` | Like above but **blocking** + `CLOEXEC` — for intentionally synchronous handoff sockets (e.g. SCM_RIGHTS setup). |
| `int net::accept_nonblock_cloexec(int listen_fd, sockaddr*, socklen_t*) noexcept` | `accept4` (Linux) or `accept`+`fcntl` (macOS), yielding a non-blocking + CLOEXEC client fd. |
| `int net::set_nonblock_cloexec(int fd) noexcept` / `set_cloexec(int fd) noexcept` | Apply the flags to an existing fd. |

**Gotcha.** Centralizing socket creation here means a new platform only needs this one file changed. If you create a raw fd outside the factories, run it through `set_nonblock_cloexec` before registering it with the reactor.

---

## 11. Scatter-gather zero-copy — `IOVec<N>` & `scattered_span`

Headers: `qbuem/io/iovec.hpp`, `qbuem/io/scattered_span.hpp`. Example: `examples/06-ipc-messaging/scatter_send/`. **This is the central zero-copy idiom of the I/O layer.**

### 11.1 `IOVec<N>`

**What it is.** A `template <size_t N> struct IOVec` holding up to `N` `iovec` entries **on the stack** (`iovec vecs[N]; size_t count`). Zero heap. You build it then hand it to `writev`/`readv`/`sendmsg`.

| Member | Signature |
|---|---|
| `push` | `void push(const void* data, size_t len) noexcept` · `void push(BufferView) noexcept` · `void push(MutableBufferView) noexcept` (asserts `count < N`) |
| `as_span` / `as_const_span` | `std::span<iovec> as_span() noexcept` / `std::span<const iovec> as_const_span() const noexcept` |
| `as_scattered` | `scattered_span as_scattered() const noexcept` |
| `total_bytes` | `size_t total_bytes() const noexcept` |
| `clear` / `empty` / `full` | `void clear()` / `bool empty()` / `bool full()` |

### 11.2 `scattered_span`

**What it is.** A non-owning **view** over an `iovec` array — internally just `std::span<const iovec>`. It is a random-access C++ range whose elements are `std::span<const std::byte>` segments, and it maps directly to POSIX scatter-gather syscall arguments. Zero allocation, zero copy, zero ownership.

| Member | Signature | Use |
|---|---|---|
| ctor | `explicit scattered_span(const IOVec<N>&)` / `explicit scattered_span(std::span<const iovec>)` / `explicit scattered_span(std::span<iovec>)` | Build a view. |
| `iov_data` | `const iovec* iov_data() const noexcept` | `writev`/`sendmsg` `iov` arg. |
| `iov_count` | `int iov_count() const noexcept` | `writev` `iovcnt` arg. |
| `as_iovec` | `std::span<const iovec> as_iovec() const noexcept` + implicit `operator std::span<const iovec>()` | Pass straight to `TcpStream::writev`. |
| `size` / `empty` / `total_bytes` | `std::size_t` / `bool` / `std::size_t` | Counts. |
| `operator[]` / `front` / `back` | `std::span<const std::byte>` | Per-segment access. |
| `begin`/`end` | range iteration | Yields `span<const std::byte>` per segment. |
| `subspan` | `scattered_span subspan(off, count = dynamic_extent) const noexcept` | Sub-view of segments. |

Plus the factory `template<size_t N, typename... Views> scattered_span make_scattered_span(IOVec<N>& storage, Views&&... views)` (each view convertible to `BufferView`; clears `storage` first).

**The pattern — one syscall, no gather allocation** (from `examples/06-ipc-messaging/scatter_send/`):

```cpp
#include <qbuem/io/iovec.hpp>
#include <qbuem/io/scattered_span.hpp>
using namespace qbuem;

IOVec<3> vec;
vec.push(status.data(),  status.size());    // "HTTP/1.1 200 OK\r\n"
vec.push(headers.data(), headers.size());   // "Content-Length: 5\r\n\r\n"
vec.push(body.data(),    body.size());      // "hello"

scattered_span scatter = vec.as_scattered();             // or scattered_span{vec}

// raw syscall — kernel gathers from 3 discontiguous locations:
::writev(fd, scatter.iov_data(), scatter.iov_count());

// or through TcpStream (implicit conversion to span<const iovec>):
co_await stream.writev(scatter);
```

```cpp
// Factory form — variadic BufferView pack into reusable stack storage:
IOVec<4> storage;
auto s = make_scattered_span(storage,
    BufferView{header.data(), header.size()},
    BufferView{body.data(),   body.size()});
co_await stream.writev(s);
```

**Gotchas.**
- **Lifetime contract (critical):** `scattered_span` does **not** own anything. The backing `IOVec<N>` *and* every buffer it points to must outlive the span. The idiom is to keep the `IOVec<N>` on the stack in the same scope as the `writev`/`sendmsg`.
- `IOVec<N>::push` asserts on overflow (`count < N`) — size `N` to your maximum segment count at compile time.
- `iov_count()` is `int` (POSIX). Segment count is bounded by `IOV_MAX` (POSIX ≥ 16; Linux 1024).
- A `writev`/`sendmsg` may be **short** under backpressure (returns fewer than `total_bytes()`); loop and `subspan`/adjust if you must send everything. Used by `Http1Handler` (`IOVec<2>{header,body}` → single `writev`) and `uds::send_fds`.

---

## 12. Other buffer & file primitives (`qbuem`, `qbuem::io`, `qbuem::zero_copy`)

### 12.1 `IOSlice` / `MutableIOSlice` — `io_slice.hpp`

Stack fat-pointers over a contiguous byte region (`const std::byte* data; size_t size`). Convert with `to_buffer_view()` (→ `BufferView`/`MutableBufferView`), `to_iovec()` (→ `iovec`), and `MutableIOSlice::as_const()`. Use them as a lightweight, conversion-friendly handle when bridging between `span`, `BufferView`, and raw `iovec` without copying.

```cpp
#include <qbuem/io/io_slice.hpp>
alignas(64) std::byte rx[4096];
qbuem::MutableIOSlice slice{rx, sizeof(rx)};
iovec iov = slice.to_iovec();          // no copy
```

### 12.2 `ReadBuf<N>` — `read_buf.hpp`

A compile-time fixed-size, cache-line-aligned (`alignas(64)`) receive ring buffer — zero heap. Cycle: `write_head()`/`writable_size()` → `commit(n)` (after socket read) → `readable()` (`BufferView`) → `consume(n)` → `compact()` to reclaim space. `size()`, `empty()`, `full()`, `reset()` round it out.

```cpp
#include <qbuem/io/read_buf.hpp>
qbuem::ReadBuf<4096> rb;
ssize_t n = ::read(fd, rb.write_head(), rb.writable_size());
if (n > 0) rb.commit(size_t(n));
qbuem::BufferView in = rb.readable();        // parse...
rb.consume(parsed);
if (rb.writable_size() < 512) rb.compact();
```

**Gotcha.** `compact()` is the only reclamation — call it when `writable_size()` runs low (it `memmove`s remaining bytes to the front). All advance methods assert on overrun.

### 12.3 `WriteBuf` — `write_buf.hpp`

A "cork" that accumulates chunks via `append(BufferView)` / `append(std::string_view)` / `append(const void*, len)` and flushes with one `writev` through `as_iovec()` (returns an `IOVec<16>` — currently one contiguous entry). `size()`, `empty()`, `clear()` (preserves capacity). Note: backed by `std::vector<std::byte>`, so `append` **may allocate** — this is a convenience/cold-path buffer, not a hot-path zero-alloc type. For hot paths, prefer `IOVec<N>` + `scattered_span` directly.

### 12.4 `BufferPool<BufSize, Count>` — `buffer_pool.hpp`

Pre-allocates `Count` cache-line-aligned buffers of `BufSize` bytes; `acquire()`/`Buffer::release()` are a lock-free intrusive free list (CAS). `acquire()` returns `nullptr` when exhausted (no exceptions). `available()` is O(1).

```cpp
#include <qbuem/io/buffer_pool.hpp>
qbuem::BufferPool<4096, 64> pool;
auto* b = pool.acquire();
if (b) { /* use b->data ... */ b->release(); }
```

**Gotcha.** Exposed to the classic lock-free-stack **ABA** problem under multi-thread contention; the safe pattern is single-reactor-thread use (the qbuem invariant). Accessing a buffer after `release()` is UB.

### 12.5 `AsyncFile` — `async_file.hpp`

RAII file handle with offset-based coroutine I/O. `static Task<Result<AsyncFile>> open(std::string_view path, int flags, mode_t mode = 0644)` (always `O_CLOEXEC`); `Task<Result<size_t>> read_at(MutableBufferView, off_t)` (`pread`, 0 == EOF); `Task<Result<size_t>> write_at(BufferView, off_t)` (`pwrite`); `Task<Result<void>> close()`; `fd()`, `is_open()`.

```cpp
#include <qbuem/io/async_file.hpp>
auto f = co_await qbuem::AsyncFile::open("data.bin", O_RDONLY);
std::array<uint8_t, 512> buf{};
auto n = co_await f->read_at(buf, 0);
```

**Gotcha.** Currently `pread`/`pwrite` are **blocking** syscalls wrapped as Tasks (offset-based, so position-independent and concurrency-safe across coroutines). For latency-critical disk I/O on Linux, prefer [`DirectFile`](#126-directfile--alignedbuffer--direct_filehpp) on a dedicated I/O thread.

### 12.6 `DirectFile` & `AlignedBuffer` — `direct_file.hpp` {#126-directfile--alignedbuffer--direct_filehpp}

`O_DIRECT | O_DSYNC` file I/O that bypasses the page cache for predictable latency (WAL / log-structured storage). `static Result<DirectFile> open(std::string_view path, bool write = false)`; `Result<size_t> read_at(std::span<std::byte>, off_t)`; `Result<size_t> write_at(std::span<const std::byte>, off_t)` (loops until full write); `close()`, `valid()`, `fd()`. Buffer/offset/size must all be **512-byte aligned** — use `AlignedBuffer<Align = 512>` (`data()`, `size()`, `span()`). Pipeline adapters `FileSink<T>` / `FileSource<T>` (both `requires std::is_trivially_copyable_v<T>`) plug into `PipelineBuilder::with_sink()` / `with_source()`, padding each record to a 512-byte stride.

```cpp
#include <qbuem/io/direct_file.hpp>
auto df = qbuem::DirectFile::open("wal.bin", /*write=*/true);
qbuem::AlignedBuffer<512> buf(4096);
auto w = df->write_at(buf.span(), 0);      // offset & size are 512-multiples
```

**Gotchas.** Synchronous/blocking — run on a dedicated I/O thread, not a reactor thread. All three alignment rules are enforced via `assert` (debug). `O_DIRECT` semantics are Linux-centric; behavior on other platforms varies.

### 12.7 Kernel zero-copy transfer — `qbuem::zero_copy` (`zero_copy.hpp`)

Coroutine wrappers that move bytes inside the kernel, no userspace copy. Each suspends on the reactor until the relevant fd is ready.

| Function | Signature | Platform |
|---|---|---|
| `sendfile` | `Task<Result<size_t>> zero_copy::sendfile(int out_fd, int in_fd, off_t offset, size_t count)` | Linux + macOS (different syscall sigs handled internally) |
| `splice` | `Task<Result<size_t>> zero_copy::splice(int in_fd, int out_fd, size_t count)` | Linux only (else `errc::function_not_supported`) |
| `send_zerocopy` | `Task<Result<size_t>> zero_copy::send_zerocopy(int sockfd, const void* buf, size_t len)` | Linux 4.14+ (`MSG_ZEROCOPY`) |
| `wait_zerocopy` | `Task<Result<void>> zero_copy::wait_zerocopy(int sockfd)` | Linux — drains the `MSG_ERRQUEUE` completion |

```cpp
#include <qbuem/io/zero_copy.hpp>
// Serve a file to a socket with no userspace copy:
auto sent = co_await qbuem::zero_copy::sendfile(sock_fd, file_fd, /*offset=*/0, file_size);
```

**Gotchas.** `send_zerocopy` requires `SO_ZEROCOPY` set first (see [socket_opts](#13-advanced-socket-options--qbuemio)) and the buffer **must not be modified** until `wait_zerocopy` confirms the kernel consumed it. `MSG_ZEROCOPY` only pays off for large buffers (>~4 KiB); small sends are faster with regular `send`. `splice` creates a temporary pipe internally.

### 12.8 Advanced socket options — `qbuem::io` (`socket_opts.hpp`) {#13-advanced-socket-options--qbuemio}

`Result<void>`-returning `setsockopt` wrappers (no exceptions). Linux-specific ones return `errc::not_supported` on old kernels (`ENOPROTOOPT`) or non-Linux.

| Function | Option / kernel | Min version |
|---|---|---|
| `io::set_incoming_cpu(int fd, int cpu)` | `SO_INCOMING_CPU` | Linux 3.19+ |
| `io::set_reuseport_cbpf(int fd, int group_size)` | `SO_ATTACH_REUSEPORT_CBPF` (queue-affinity steering) | Linux 4.5+ |
| `io::enable_tcp_migrate_req(int fd)` | `TCP_MIGRATE_REQ` (graceful worker restart) | Linux 5.14+ |
| `io::set_tcp_fastopen(int fd, int qlen = 10)` | `TCP_FASTOPEN` | Linux 3.7+ |
| `io::set_zerocopy(int fd)` | `SO_ZEROCOPY` | Linux 4.14+ |
| `io::set_reuseport(int fd)` / `io::set_reuseaddr(int fd)` | `SO_REUSEPORT` / `SO_REUSEADDR` | portable |

```cpp
#include <qbuem/io/socket_opts.hpp>
if (auto r = qbuem::io::set_zerocopy(fd); !r) { /* fall back to regular send */ }
qbuem::io::set_incoming_cpu(fd, 0);   // pin a SO_REUSEPORT socket to CPU 0
```

**Gotcha.** Treat every Linux-advanced option as **best effort** — check the `Result` and fall back; `errc::not_supported` is expected on older kernels, ARM boards with stripped configs, or macOS. `set_tcp_fastopen` must be called **before** `listen()`, and TFO also needs the system sysctl enabled.

---

## 13. Shared-memory IPC — `qbuem::shm`

The SHM model is the highest-throughput IPC in the library: a `memfd`/`shm_open`-backed segment with a lock-free Vyukov-style ring; the consumer reads the data arena **in place** (zero copy). Target p99 latency ~150 ns. Example: `examples/06-ipc-messaging/shm_channel/` and the composite `examples/06-ipc-messaging/ipc_pipeline/`.

### 13.1 `SHMChannel<T>` — cross-process zero-copy channel {#13-shmchannelt--cross-process-zero-copy-channel}

Header: `qbuem/shm/shm_channel.hpp`.

**What it is.** `template <typename T> class SHMChannel` (with `static_assert(std::is_trivially_copyable_v<T>)`) — an MPMC channel over shared memory. The producer CAS-acquires a ring slot, `memcpy`s `T` into the data arena, and commits a sequence number; the consumer reads the arena directly.

**When to use it.** Bulk, high-rate, low-latency data between processes on one host (order flow, sensor streams, frame buffers). Within a single process across threads, an `AsyncChannel<T>` (or `SHMBus` `LOCAL_ONLY`) is cheaper (~50 ns). For passing *control* (fds, credentials) use [`qbuem::uds`](#5-uds-fd-passing--qbuemuds).

**API.**

| Member | Signature |
|---|---|
| `create` | `static Result<Ptr> create(std::string_view name, size_t capacity) noexcept` (producer; rounds capacity up to a power of two) |
| `open` | `static Result<Ptr> open(std::string_view name) noexcept` (consumer; validates magic) |
| `unlink` | `static Result<void> unlink(std::string_view name) noexcept` (remove `/dev/shm` entry) |
| `send` | `Task<Result<void>> send(const T& msg) noexcept` (co_await backpressure when full; `errc::broken_pipe` if closed) |
| `try_send` | `bool try_send(const T& msg) noexcept` (false if full or closed) |
| `recv` | `Task<std::optional<const T*>> recv() noexcept` (zero-copy ptr; `nullopt` when closed) |
| `try_recv` | `std::optional<const T*> try_recv() noexcept` (`nullopt` when empty) |
| `close` / `is_open` | `void close()` / `bool is_open() const` |
| `size_approx` / `capacity` | `size_t … const noexcept` |

`Ptr` is `std::unique_ptr<SHMChannel<T>>`. Helper `size_t calc_segment_size(size_t capacity, size_t msg_size, bool envelope = false)` computes the page-aligned segment size.

```cpp
#include <qbuem/shm/shm_channel.hpp>
using namespace qbuem::shm;

struct SensorReading { float temp; float humidity; uint32_t id; };
static_assert(std::is_trivially_copyable_v<SensorReading>);

// Producer process
auto p = SHMChannel<SensorReading>::create("sensor_ch", 8);   // -> /dev/shm/sensor_ch
p.value()->try_send(SensorReading{20.f, 55.f, 0});            // or: co_await (*p)->send(r)

// Consumer process
auto c = SHMChannel<SensorReading>::open("sensor_ch");
if (auto v = c.value()->try_recv()) {           // std::optional<const T*>
    const SensorReading* r = *v;                // points INTO the data arena
}
// On shutdown:
SHMChannel<SensorReading>::unlink("sensor_ch");
```

**Gotchas.**
- `T` **must** be `std::is_trivially_copyable_v` (enforced) — no `std::string`, `std::vector`, pointers-with-meaning, or vtables; only POD-like records survive a cross-process `memcpy`.
- `recv`/`try_recv` return a `const T*` valid **only until the next `recv` on the same channel instance** (it points into a per-instance copy buffer / the arena). Copy out if you need to retain it past the next receive.
- `create` truncates+initializes the segment; `open` requires it to already exist (`errc::no_such_file_or_directory`) and validates the magic. Name conventions: leading `/` is added automatically; the OS entry persists until `unlink` — clean up to avoid stale `/dev/shm` files.
- The current build's futex wait/wake (`IORING_OP_FUTEX_*`) falls back to a **1 ms poll** (`AsyncSleep{1}`) when io_uring futex is unavailable, so `send`/`recv` blocking under contention is poll-paced; the `try_*` fast paths are unaffected and remain truly lock-free.
- Capacity is rounded **up** to a power of two; check `capacity()` for the real slot count.

### 13.2 `SHMBus` — unified intra/inter-process pub/sub

Header: `qbuem/shm/shm_bus.hpp`.

**What it is.** A topic bus that hides whether a topic is `LOCAL_ONLY` (backed by `AsyncChannel<T>`, ~50 ns, same process) or `SYSTEM_WIDE` (backed by `SHMChannel<T>`, ~150 ns, cross-process) behind one API. Up to `kMaxTopics = 64` topics per bus.

**When to use it.** You want one publish/subscribe surface and may relocate a topic across the process boundary later without rewriting call sites. For a single fixed cross-process stream, `SHMChannel<T>` directly is leaner.

**API.**

| Member | Signature |
|---|---|
| `declare` | `template<typename T> bool declare(std::string_view name, TopicScope scope, size_t cap = 1024)` |
| `publish` | `template<typename T> Task<Result<void>> publish(std::string_view name, const T& msg)` |
| `try_publish` | `template<typename T> bool try_publish(std::string_view name, const T& msg)` |
| `subscribe` | `template<typename T> std::unique_ptr<ISubscription<T>> subscribe(std::string_view name)` |
| `has_topic` / `topic_count` | `bool` / `size_t` |

`enum class TopicScope : uint8_t { LOCAL_ONLY, SYSTEM_WIDE }`. `ISubscription<T>`: `Task<std::optional<const T*>> recv()`, `std::optional<const T*> try_recv()`, `std::string_view topic()`, `TopicScope scope()`.

```cpp
#include <qbuem/shm/shm_bus.hpp>
using namespace qbuem::shm;

struct OrderEvent { int id; double price; int qty; };
static_assert(std::is_trivially_copyable_v<OrderEvent>);

SHMBus bus;
bus.declare<OrderEvent>("trading.orders", TopicScope::LOCAL_ONLY, 64);
auto sub = bus.subscribe<OrderEvent>("trading.orders");

bus.try_publish("trading.orders", OrderEvent{1001, 250.5, 100});
if (auto m = sub->try_recv()) { /* (*m)->id == 1001 */ }
// async form: co_await bus.publish(...); while (auto m = co_await sub->recv()) { ... }
```

**Gotchas.** `declare` returns `false` on duplicate name or when `kMaxTopics` is exceeded — check it. `SYSTEM_WIDE` requires `std::is_trivially_copyable_v<T>` (compile-time enforced); `LOCAL_ONLY` does not. Type safety across `declare`/`publish`/`subscribe` is a size+alignment "type id" (`(sizeof<<32)|alignof`) — a mismatch yields `errc::invalid_argument` / `nullptr`; declare and use each topic with one consistent `T`. Recommended one `SHMBus` per process.

### 13.3 Pipeline adapters — `SHMSource<T>` / `SHMSink<T>`

Also in `shm_bus.hpp`. These bridge an `SHMChannel<T>` into the pipeline API: `SHMSource<T>` opens an existing channel and feeds it into `PipelineBuilder::with_source()`; `SHMSink<T>` creates a channel and drains pipeline output via `with_sink()`. Both copy the channel name internally (no dangling `string_view`).

| Type | Key members |
|---|---|
| `SHMSource<T>` | ctor `explicit SHMSource(std::string_view name)`; `Result<void> init()` (opens); `Task<std::optional<const T*>> next()` |
| `SHMSink<T>` | ctor `explicit SHMSink(std::string_view name, size_t capacity = 1024)`; `Result<void> init()` (creates); `Task<Result<void>> sink(const T& msg)` |

```cpp
// From examples/06-ipc-messaging/ipc_pipeline/: SHMChannel -> SHMSource -> StaticPipeline
auto pipeline = PipelineBuilder<RawOrder, RawOrder>{}
    .with_source(SHMSource<RawOrder>("trading.raw_orders"))
    .add<RawOrder>(validate_stage)
    .build();
```

**Gotcha.** `SHMSource::init()` requires the channel to already exist (a producer must have `create`d it); the framework calls `init()` before the first `next()`/`sink()`.

### 13.4 Futex synchronization — `FutexWord`, `FutexSync`, `FutexMutex`, `FutexSemaphore`

Header: `qbuem/shm/futex_sync.hpp`. **Cross-process** synchronization primitives meant to live **inside** a shared-memory segment.

- `struct FutexWord` — a 4-byte, 4-aligned process-shared atomic (`load`/`store`/`compare_exchange`/`fetch_add`/`fetch_sub`). Place via placement-new in SHM.
- `class FutexSync` — `static Task<void> wait(FutexWord&, uint32_t expected, Reactor&, uint64_t timeout_ns = 0)` (co_await; returns immediately if value already differs), `static int wake(FutexWord&, uint32_t count = 1)`, `wake_all`, `static bool has_uring_futex()`.
- `class FutexMutex` — inter-process mutex; `Task<LockGuard> lock(Reactor&)` (RAII unlock), `std::optional<LockGuard> try_lock()`, `unlock()`, `is_locked()`.
- `class FutexSemaphore` — counting semaphore; ctor `explicit FutexSemaphore(uint32_t initial = 0)`, `Task<void> acquire(Reactor&)`, `bool try_acquire()`, `release(uint32_t = 1)`, `value()`.

```cpp
#include <qbuem/shm/futex_sync.hpp>
using namespace qbuem::shm;

// FutexWord placed in a shared segment:
auto seg = SHMSegment::create("ctrl", 4096);
auto* fw = new (seg->base()) FutexWord(0);

// Producer (any thread/process):
fw->store(1);
FutexSync::wake(*fw, 1);

// Consumer coroutine:
co_await FutexSync::wait(*fw, 0, reactor);   // wakes when fw != 0
```

**Gotchas.** These only make sense placed in shared memory (process-shared, no `FUTEX_PRIVATE_FLAG`). The io_uring futex path needs **Linux 6.7+**; otherwise it falls back to `futex(2)` syscalls (`has_uring_futex()` tells you which). `wait` is a no-op fast return if the word already differs from `expected` — the standard futex usage pattern. `FutexMutex`/`FutexSemaphore` are non-copyable and must not be moved out of their SHM home.

### 13.5 Portability — `shm_compat.hpp`

A transparent shim: on Android (Bionic, which lacks `shm_open`/`shm_unlink`) it emulates them with a file under `/data/local/tmp`; on every other POSIX platform it just includes `<sys/mman.h>`. You never call it directly — `SHMChannel`/`SHMSegment` include it so the same code compiles on Linux x86_64/ARM64, macOS aarch64, and Android.

---

## 14. Choosing the right transport

| Need | Use |
|---|---|
| Reliable byte stream, cross-host | `TcpStream` / `TcpListener` |
| Connectionless datagrams | `UdpSocket` |
| Very high UDP packet rate | `UdpMmsgSocket` (batched syscalls) |
| One-to-many fan-out on a LAN | `MulticastSocket` |
| Ordered + reliable over UDP | `RudpSocket` |
| Local stream IPC | `UnixSocket` |
| Pass fds / verify peer identity locally | `qbuem::uds::send_fds` / `get_peer_credentials` |
| Bulk zero-copy data between local processes | `SHMChannel<T>` (or `SHMBus` `SYSTEM_WIDE`) |
| Pub/sub that may move across the process boundary | `SHMBus` |
| Cross-process locks / counting | `FutexMutex` / `FutexSemaphore` in SHM |
| Header+body in one syscall, no gather copy | `IOVec<N>` + `scattered_span` + `writev` |
| Serve a file to a socket with no userspace copy | `qbuem::zero_copy::sendfile` |
| Page-cache-bypassing durable writes | `DirectFile` + `AlignedBuffer` |

**Source map (absolute paths).** Net: `/Users/goodboy/Projects/qbuem/qbuem-stack/include/qbuem/net/`. I/O: `/Users/goodboy/Projects/qbuem/qbuem-stack/include/qbuem/io/`. SHM: `/Users/goodboy/Projects/qbuem/qbuem-stack/include/qbuem/shm/`. Runnable examples: `/Users/goodboy/Projects/qbuem/qbuem-stack/examples/02-network/` and `/Users/goodboy/Projects/qbuem/qbuem-stack/examples/06-ipc-messaging/`.
