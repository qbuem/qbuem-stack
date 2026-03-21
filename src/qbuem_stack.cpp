#include <qbuem/qbuem_stack.hpp>
#include <qbuem/http/parser.hpp>
#include <qbuem/middleware/static_files.hpp>

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <ctime>
#include <format>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>
#include <variant>

namespace qbuem {

// ─── Date header cache (RFC 7231 §7.1.1.2) ──────────────────────────────────
// Refreshed at most once per second; lock held only on update (rare path).
namespace {

std::atomic<std::time_t> g_date_ts{0};
char                      g_date_buf[48] = {}; // NOLINT(modernize-avoid-c-arrays)
std::mutex                g_date_mu;

// vDSO fast monotonic second — no syscall overhead on Linux (CLOCK_MONOTONIC_COARSE
// is served from the vDSO page without entering kernel mode on glibc 2.17+).
static inline std::time_t fast_now() noexcept {
#ifdef __linux__
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
  // Monotonic time gives elapsed seconds; add wall-clock epoch offset cached once.
  // For the Date header we need wall-clock seconds, so fall back to CLOCK_REALTIME_COARSE.
  clock_gettime(CLOCK_REALTIME_COARSE, &ts);
  return static_cast<std::time_t>(ts.tv_sec);
#else
  return std::time(nullptr);
#endif
}

std::string_view cached_http_date() noexcept {
  std::time_t now = fast_now();
  if (g_date_ts.load(std::memory_order_relaxed) != now) {
    std::lock_guard lk(g_date_mu);
    if (g_date_ts.load(std::memory_order_relaxed) != now) {
      std::tm tm{};
      gmtime_r(&now, &tm);
      std::strftime(g_date_buf, sizeof(g_date_buf),
                    "%a, %d %b %Y %H:%M:%S GMT", &tm);
      g_date_ts.store(now, std::memory_order_release);
    }
  }
  return g_date_buf;
}

// ─── write_all ──────────────────────────────────────────────────────────────
void write_all(int fd, const std::string &data) {
  const char *ptr = data.data();
  ssize_t rem = static_cast<ssize_t>(data.size());
  while (rem > 0) {
    ssize_t n = write(fd, ptr, static_cast<size_t>(rem));
    if (n <= 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    ptr += n;
    rem -= n;
  }
}

// ─── send_file_body ──────────────────────────────────────────────────────────
// Zero-copy file-to-socket transfer using platform sendfile().
#ifdef __linux__
static void send_file_body(int sock_fd, std::string_view path, size_t size) noexcept {
  int file_fd = ::open(path.data(), O_RDONLY | O_CLOEXEC);
  if (file_fd < 0) return;
  off_t offset = 0;
  size_t remaining = size;
  while (remaining > 0) {
    ssize_t n = ::sendfile(sock_fd, file_fd, &offset,
                           std::min(remaining, static_cast<size_t>(0x7fff'ffff)));
    if (n <= 0) {
      if (errno == EINTR) continue;
      break;
    }
    remaining -= static_cast<size_t>(n);
  }
  ::close(file_fd);
}
#elif defined(__APPLE__)
// macOS sendfile(2) — different signature: off_t *len is both input+output.
static void send_file_body(int sock_fd, std::string_view path, size_t size) noexcept {
  int file_fd = ::open(path.data(), O_RDONLY | O_CLOEXEC);
  if (file_fd < 0) return;
  off_t offset    = 0;
  off_t remaining = static_cast<off_t>(size);
  while (remaining > 0) {
    off_t len = remaining;
    int r = ::sendfile(file_fd, sock_fd, offset, &len, nullptr, 0);
    if (len > 0) { offset += len; remaining -= len; }
    if (r < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;
      break;
    }
  }
  ::close(file_fd);
}
#endif

// ─── writev_response ─────────────────────────────────────────────────────────
// Send HTTP header + body in a single writev() syscall — no heap concat.
// Falls back to write_all for the body if writev() is short.
void writev_response(int fd, const std::string &hdr, std::string_view body) {
  struct iovec iov[2]; // NOLINT(modernize-avoid-c-arrays)
  iov[0].iov_base = const_cast<char *>(hdr.data());
  iov[0].iov_len  = hdr.size();
  iov[1].iov_base = const_cast<char *>(body.data());
  iov[1].iov_len  = body.size();

  int iov_cnt = body.empty() ? 1 : 2;
  ssize_t total = static_cast<ssize_t>(hdr.size() + body.size());
  ssize_t sent  = 0;
  int     idx   = 0;

  while (sent < total) {
    ssize_t n = writev(fd, iov + idx, iov_cnt - idx);
    if (n <= 0) {
      if (errno == EINTR) continue;
      break;
    }
    sent += n;
    // Advance iovec pointers for partial writes
    ssize_t remaining = n;
    while (idx < iov_cnt && remaining > 0) {
      if (static_cast<ssize_t>(iov[idx].iov_len) <= remaining) {
        remaining -= static_cast<ssize_t>(iov[idx].iov_len);
        iov[idx].iov_len = 0;
        ++idx;
      } else {
        iov[idx].iov_base = static_cast<char *>(iov[idx].iov_base) + remaining;
        iov[idx].iov_len -= static_cast<size_t>(remaining);
        remaining = 0;
      }
    }
  }
}

// ─── Graceful shutdown ───────────────────────────────────────────────────────
static App *g_app_instance = nullptr;

static void on_shutdown_signal(int) {
  if (g_app_instance != nullptr)
    g_app_instance->stop();
}

// ─── Per-connection keep-alive state ────────────────────────────────────────
// ConnCtx is heap-allocated per connection (shared_ptr).
// Hot fields (timer IDs, handled count, sent_100) are grouped first to fit
// within the first cache line (64 B).  The strings (buf, client_ip) are cold
// by comparison and allowed to fall into the second line.
struct ConnCtx {
  // ── Hot fields (accessed on every reactor wakeup) ── ~28 B ──────────────
  int  idle_timer_id  = -1;  // current idle timer (-1 = none)
  int  read_timer_id  = -1;  // per-request read timeout timer (-1 = none)
  int  handled        = 0;   // requests handled on this connection
  bool sent_100       = false; // already sent 100 Continue for current request

  // ── Cold fields (written once on accept, read rarely) ────────────────────
  std::string buf;           // partial-read accumulation buffer
  std::string client_ip;     // remote peer address (dotted-decimal or IPv6)

  static constexpr int MAX_REQUESTS    = 100;
  static constexpr int IDLE_TIMEOUT_MS = 30'000; // 30 s
  // Max time to receive a complete request after the first byte arrives.
  // Closes Slowloris-style attack vectors (very slow header delivery).
  static constexpr int READ_TIMEOUT_MS = 10'000; // 10 s
};

// ─── Async middleware chain runner ───────────────────────────────────────────
// Executes the slice mws[idx..end) as a coroutine with next() semantics.
// req_ptr / res_ptr are shared so each middleware can inspect the response
// after co_await next() returns (post-processing pattern).
// tail() is invoked once all middlewares have run; it executes the route handler
// and finalizes the response.
static Task<bool> run_mw_chain(
    const std::vector<AnyMiddleware> *mws, // pointer to app-owned vector (stable)
    size_t idx,
    std::shared_ptr<Request>  req_ptr,
    std::shared_ptr<Response> res_ptr,
    std::function<Task<void>(std::shared_ptr<Request>, std::shared_ptr<Response>)> tail)
{
  if (idx >= mws->size()) {
    co_await tail(req_ptr, res_ptr);
    co_return true;
  }

  const auto &mw = (*mws)[idx];
  if (std::holds_alternative<Middleware>(mw)) {
    if (!std::get<Middleware>(mw)(*req_ptr, *res_ptr)) co_return false;
    co_return co_await run_mw_chain(mws, idx + 1, req_ptr, res_ptr, tail);
  } else {
    // AsyncMiddleware: provide next() as a coroutine that runs the rest of chain.
    // Capturing req_ptr/res_ptr by value keeps them alive across suspension.
    NextFn next_fn = [mws, idx, req_ptr, res_ptr, tail]() mutable -> Task<void> {
      co_await run_mw_chain(mws, idx + 1, req_ptr, res_ptr, tail);
    };
    co_return co_await std::get<AsyncMiddleware>(mw)(*req_ptr, *res_ptr, next_fn);
  }
}

} // namespace

// ─── App ─────────────────────────────────────────────────────────────────────
App::App(size_t thread_count) : dispatcher_(thread_count) {
  std::cout << "qbuem-stack v" << Version::string << " initializing ("
            << thread_count << " reactor threads)..." << std::endl;
}

void App::use(Middleware mw) { router_.use(std::move(mw)); } // NOLINT(performance-unnecessary-value-param)
void App::use_async(AsyncMiddleware mw) { router_.use_async(std::move(mw)); }

void App::on_error(ErrorHandler handler) { error_handler_ = std::move(handler); } // NOLINT(performance-unnecessary-value-param)

void App::get(std::string_view path, HandlerVariant handler) { // NOLINT(performance-unnecessary-value-param)
  router_.add_route(Method::Get, path, std::move(handler));
}
void App::post(std::string_view path, HandlerVariant handler) { // NOLINT(performance-unnecessary-value-param)
  router_.add_route(Method::Post, path, std::move(handler));
}
void App::put(std::string_view path, HandlerVariant handler) { // NOLINT(performance-unnecessary-value-param)
  router_.add_route(Method::Put, path, std::move(handler));
}
void App::del(std::string_view path, HandlerVariant handler) { // NOLINT(performance-unnecessary-value-param)
  router_.add_route(Method::Delete, path, std::move(handler));
}
void App::patch(std::string_view path, HandlerVariant handler) { // NOLINT(performance-unnecessary-value-param)
  router_.add_route(Method::Patch, path, std::move(handler));
}
void App::head(std::string_view path, HandlerVariant handler) { // NOLINT(performance-unnecessary-value-param)
  router_.add_route(Method::Head, path, std::move(handler));
}
void App::options(std::string_view path, HandlerVariant handler) { // NOLINT(performance-unnecessary-value-param)
  router_.add_route(Method::Options, path, std::move(handler));
}
void App::serve_static(std::string_view url_prefix, std::string_view root_dir) {
  router_.add_prefix_route(
      Method::Get, url_prefix,
      Handler([prefix = std::string(url_prefix),
               root   = std::string(root_dir)](const Request &req,
                                               Response &res) {
        // The router stores the path suffix (after the prefix) in param "**".
        std::string rel = std::string(req.param("**"));

        // Normalize: ensure leading '/' is stripped.
        while (!rel.empty() && rel[0] == '/') rel = rel.substr(1);

        // Index file fallback for empty suffix.
        if (rel.empty()) rel = "index.html";

        // ── Local path traversal guard ──────────────────────────────
        // The server-level guard already caught %2e%2e before dispatch,
        // but apply a defence-in-depth check on the reconstructed path.
        if (rel.find("..") != std::string::npos) {
          res.status(400).body("Bad Request");
          return;
        }

        std::string fs_path = root + "/" + rel;
        middleware::serve_file(fs_path, res);
      }));
}

void App::health_check(std::string_view path) {
  router_.add_route(Method::Get, path,
    Handler([](const Request &, Response &res) {
      res.status(200)
         .header("Content-Type", "application/json")
         .body("{\"status\":\"ok\"}");
    }));
}

void App::health_check_detailed(std::string_view path) {
  router_.add_route(Method::Get, path,
    Handler([this](const Request &, Response &res) {
      auto m = snapshot_metrics();
      auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - start_time_).count();
      std::string body = "{\"status\":\"ok\","
          "\"connections\":" + std::to_string(m.active_connections) + ","
          "\"uptime_s\":"    + std::to_string(uptime) + ","
          "\"requests_total\":" + std::to_string(m.requests_total) + "}";
      res.status(200).header("Content-Type", "application/json").body(body);
    }));
}

void App::liveness_endpoint(std::string_view path) {
  router_.add_route(Method::Get, path,
    Handler([](const Request &, Response &res) {
      res.status(200)
         .header("Content-Type", "application/json")
         .body("{\"alive\":true}");
    }));
}

void App::readiness_endpoint(std::string_view path) {
  router_.add_route(Method::Get, path,
    Handler([this](const Request &, Response &res) {
      if (draining_.load(std::memory_order_relaxed)) {
        res.status(503)
           .header("Content-Type", "application/json")
           .header("Retry-After", "5")
           .body("{\"ready\":false,\"reason\":\"draining\"}");
      } else {
        res.status(200)
           .header("Content-Type", "application/json")
           .body("{\"ready\":true}");
      }
    }));
}

// ─── Metrics ─────────────────────────────────────────────────────────────────

Metrics App::snapshot_metrics() const {
  return {
      cnt_requests_.load(std::memory_order_relaxed),
      cnt_errors_.load(std::memory_order_relaxed),
      cnt_active_.load(std::memory_order_relaxed),
      cnt_bytes_sent_.load(std::memory_order_relaxed),
  };
}

void App::metrics_endpoint(std::string_view path) {
  router_.add_route(
      Method::Get, path,
      Handler([this](const Request &, Response &res) {
        auto m = snapshot_metrics();
        std::string body =
            "qbuem_requests_total " + std::to_string(m.requests_total) + "\n" +
            "qbuem_errors_total " + std::to_string(m.errors_total) + "\n" +
            "qbuem_active_connections " +
            std::to_string(m.active_connections) + "\n" +
            "qbuem_bytes_sent " + std::to_string(m.bytes_sent) + "\n";
        res.status(200)
           .header("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
           .body(body);
      }));
}

// ─── Access log ───────────────────────────────────────────────────────────────

void App::set_access_logger(
    std::function<void(std::string_view, std::string_view, int, long)> fn) {
  logger_ = std::move(fn);
}

void App::set_structured_logger(
    std::function<void(const StructuredLogRecord &)> fn) {
  structured_logger_ = std::move(fn);
}

void App::enable_structured_log() {
  structured_logger_ = [](const StructuredLogRecord &r) {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);
    auto line = std::format(
        "{{\"ts\":\"{}\",\"method\":\"{}\",\"path\":\"{}\","
        "\"status\":{},\"duration_us\":{},"
        "\"remote_addr\":\"{}\",\"request_id\":\"{}\","
        "\"trace_id\":\"{}\"}}\n",
        ts, r.method, r.path, r.status, r.duration_us,
        r.remote_addr, r.request_id, r.trace_id);
    std::fputs(line.c_str(), stderr);
  };
}

void App::enable_access_log() {
  logger_ = [](std::string_view method, std::string_view path, int status,
               long duration_us) {
    // Format: [YYYY-MM-DDTHH:MM:SSZ] METHOD /path STATUS Xµs
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);
    auto line = std::format("[{}] {} {} {} {}µs\n",
                            ts, method, path, status, duration_us);
    std::fputs(line.c_str(), stderr);
  };
}

void App::enable_json_log() {
  logger_ = [](std::string_view method, std::string_view path, int status,
               long duration_us) {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);
    auto line = std::format(
        "{{\"ts\":\"{}\",\"method\":\"{}\","
        "\"path\":\"{}\",\"status\":{},\"duration_us\":{}}}\n",
        ts, method, path, status, duration_us);
    std::fputs(line.c_str(), stderr);
  };
}

void App::set_max_connections(uint64_t max) {
  max_connections_.store(max, std::memory_order_relaxed);
}

// ─── App lifecycle ────────────────────────────────────────────────────────────

void App::stop(int drain_timeout_ms) {
  // Step 1: enter drain mode → readiness probe returns 503 immediately.
  draining_.store(true, std::memory_order_release);

  // Step 2: close all listen sockets so the kernel stops queueing new SYNs.
  // Reactor callbacks for the listen fds will return EBADF / POLLHUP and
  // auto-remove themselves from the event loop.
  for (int fd : listen_fds_) {
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
  }
  listen_fds_.clear();

  if (drain_timeout_ms <= 0) {
    // Immediate shutdown — matches the original behaviour.
    dispatcher_.stop();
    return;
  }

  // Step 3: wait for active connections to drain naturally.
  // Poll with 10 ms granularity; reactors keep running so idle-timers fire.
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(drain_timeout_ms);
  while (cnt_active_.load(std::memory_order_acquire) > 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // Step 4: stop reactor threads regardless of remaining active connections.
  dispatcher_.stop();
}

// ─── make_listen_socket ───────────────────────────────────────────────────────
// Creates, configures, binds and listens a single TCP socket for the given port.
// With SO_REUSEPORT every socket can share the same port; the kernel load-balances
// incoming connections across all sockets without lock contention (accept storm free).
static int make_listen_socket(int port, bool ipv6) noexcept {
  int af = ipv6 ? AF_INET6 : AF_INET;
  int fd  = ::socket(af, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  int opt = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

#if defined(__linux__) && defined(TCP_FASTOPEN)
  int tfo_qlen = 128;
  setsockopt(fd, IPPROTO_TCP, TCP_FASTOPEN, &tfo_qlen, sizeof(tfo_qlen));
#endif

  if (ipv6) {
    int v6only = 0;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
  }

#ifdef __linux__
  int defer_secs = 5;
  setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &defer_secs, sizeof(defer_secs));
#endif

  if (ipv6) {
    struct sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    a.sin6_addr   = in6addr_any;
    a.sin6_port   = htons(static_cast<uint16_t>(port));
    if (bind(fd, reinterpret_cast<struct sockaddr *>(&a), sizeof(a)) < 0) {
      ::close(fd); return -1;
    }
  } else {
    struct sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port        = htons(static_cast<uint16_t>(port));
    if (bind(fd, reinterpret_cast<struct sockaddr *>(&a), sizeof(a)) < 0) {
      ::close(fd); return -1;
    }
  }

  if (::listen(fd, 1024) < 0) { ::close(fd); return -1; }
  fcntl(fd, F_SETFL, O_NONBLOCK);
  return fd;
}

Result<void> App::listen(int port, bool ipv6) {
  start_time_ = std::chrono::steady_clock::now();
  draining_.store(false, std::memory_order_relaxed);
  // ── SIGTERM / SIGINT → graceful shutdown ──────────────────────────────────
  g_app_instance = this;
  std::signal(SIGTERM, on_shutdown_signal);
  std::signal(SIGINT,  on_shutdown_signal);

  // ── SO_REUSEPORT per-reactor listening sockets ────────────────────────────
  // Create one listening socket per reactor thread.  The kernel distributes
  // incoming SYNs across all N sockets without a shared accept lock, eliminating
  // the "accept storm" / "thundering herd" problem seen with a single socket.
  // Each reactor thread calls accept() only on its own dedicated socket, so
  // there is zero inter-thread contention in the accept path.
  size_t N = dispatcher_.thread_count();
  std::vector<int> server_fds;
  server_fds.reserve(N);

  for (size_t i = 0; i < N; ++i) {
    int fd = make_listen_socket(port, ipv6);
    if (fd < 0) {
      for (int sfd : server_fds) ::close(sfd);
      return std::unexpected(std::make_error_code(std::errc::address_in_use));
    }
    server_fds.push_back(fd);
  }

  std::cout << "qbuem-stack v" << Version::string
            << " listening on http" << (ipv6 ? "://[::]:" : "://0.0.0.0:")
            << port << " (" << N << " reactors, SO_REUSEPORT)" << std::endl;

  // ── Accept loop — one accept-callback per reactor ─────────────────────────
  for (size_t i = 0; i < N; ++i) {
    auto listen_res = dispatcher_.register_listener_at(
        server_fds[i], i, [this](int lfd) {
        Reactor *reactor = Reactor::current();

        struct sockaddr_storage client_addr{};
        socklen_t client_len = sizeof(client_addr);
        // Drain mode: stop accepting new connections.
        if (draining_.load(std::memory_order_acquire)) return;

        int client_fd =
            accept(lfd, reinterpret_cast<struct sockaddr *>(&client_addr),
                   &client_len);
        if (client_fd == -1) {
          if (errno != EAGAIN && errno != EWOULDBLOCK)
            std::cerr << "[ERROR] accept: " << strerror(errno) << std::endl;
          return;
        }

        // Extract remote peer IP address for use in Request::remote_addr().
        char peer_ip[INET6_ADDRSTRLEN] = {}; // NOLINT(modernize-avoid-c-arrays)
        if (client_addr.ss_family == AF_INET6) {
          const auto *a6 = reinterpret_cast<const struct sockaddr_in6 *>(&client_addr);
          inet_ntop(AF_INET6, &a6->sin6_addr, peer_ip, sizeof(peer_ip));
        } else {
          const auto *a4 = reinterpret_cast<const struct sockaddr_in *>(&client_addr);
          inet_ntop(AF_INET, &a4->sin_addr, peer_ip, sizeof(peer_ip));
        }

        fcntl(client_fd, F_SETFL, O_NONBLOCK);

        // ── Connection limit guard ────────────────────────────────────────
        // If max_connections_ is set and active count has reached the limit,
        // send 503 Service Unavailable and close immediately.
        {
          uint64_t mx = max_connections_.load(std::memory_order_relaxed);
          if (mx > 0 && cnt_active_.load(std::memory_order_relaxed) >= mx) {
            static constexpr std::string_view kOverload =
                "HTTP/1.1 503 Service Unavailable\r\n"
                "Content-Type: application/json\r\n"
                "Retry-After: 1\r\n"
                "Content-Length: 37\r\n"
                "Connection: close\r\n\r\n"
                "{\"error\":\"too many connections\"}    ";
            // best-effort send (ignore partial write — connection closing anyway)
            [[maybe_unused]] auto _ =
                ::write(client_fd, kOverload.data(), kOverload.size() - 4);
            ::close(client_fd);
            return;
          }
        }

#ifdef SO_NOSIGPIPE
        // macOS: prevent SIGPIPE on write to closed socket.
        int nosig = 1;
        setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
#endif

        // Disable Nagle algorithm — send data immediately without waiting to
        // accumulate a full segment.  Critical for request-response latency.
        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY,
                   &nodelay, sizeof(nodelay));

#ifdef __linux__
        // TCP_QUICKACK: send ACKs immediately instead of waiting for delayed
        // ACK timer (40 ms).  Re-applied each time we write (kernel resets it).
        int quickack = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_QUICKACK,
                   &quickack, sizeof(quickack));

#  ifdef SO_BUSY_POLL
        // SO_BUSY_POLL: busy-poll for up to N microseconds before blocking.
        // Reduces per-packet receive latency at the cost of slightly higher CPU
        // usage.  50 µs is a good balance for low-latency HTTP services.
        int busy_poll_us = 50;
        setsockopt(client_fd, SOL_SOCKET, SO_BUSY_POLL,
                   &busy_poll_us, sizeof(busy_poll_us));
#  endif
#endif

        // SO_SNDTIMEO: kernel-enforced write timeout.
        // Prevents slow clients from blocking a reactor thread indefinitely
        // when the send buffer fills up.  5 s is generous for LAN peers.
        {
          struct timeval tv{5, 0}; // 5 seconds
          setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }

        // Per-connection shared state (lives as long as any callback holds it).
        // Custom deleter: decrement active-connection counter when the last
        // reference is released (which happens after all callbacks for this
        // fd are removed from the reactor).
        cnt_active_.fetch_add(1, std::memory_order_relaxed);
        auto ctx = std::shared_ptr<ConnCtx>(new ConnCtx{},
            [this](ConnCtx *c) {
              cnt_active_.fetch_sub(1, std::memory_order_relaxed);
              delete c;
            });
        ctx->client_ip = peer_ip;

        // arm_idle: cancel current idle timer and start a fresh one.
        // All callbacks for a given fd run on the same reactor thread,
        // so there are no data races on ctx.
        auto arm_idle = [ctx, reactor, client_fd]() {
          if (ctx->idle_timer_id != -1) {
            reactor->unregister_timer(ctx->idle_timer_id);
            ctx->idle_timer_id = -1;
          }
          auto tres = reactor->register_timer(
              ConnCtx::IDLE_TIMEOUT_MS, [reactor, client_fd, ctx](int) {
                ctx->idle_timer_id = -1;
                reactor->unregister_event(client_fd, EventType::Read);
                close(client_fd);
              });
          if (tres)
            ctx->idle_timer_id = *tres;
        };

        // arm_read_timeout: start per-request read timer (not reset on partial
        // reads).  Fires if a full request is not received within READ_TIMEOUT_MS.
        // Call once when the first byte of a new request arrives.
        auto arm_read_timeout = [ctx, reactor, client_fd]() {
          if (ctx->read_timer_id != -1) return; // already armed
          auto tres = reactor->register_timer(
              ConnCtx::READ_TIMEOUT_MS, [reactor, client_fd, ctx](int) {
                ctx->read_timer_id = -1;
                // Send 408 and close
                Response tout;
                tout.status(408)
                    .header("Connection", "close")
                    .body("Request Timeout");
                write_all(client_fd, tout.serialize());
                reactor->unregister_event(client_fd, EventType::Read);
                close(client_fd);
              });
          if (tres) ctx->read_timer_id = *tres;
        };

        auto cancel_read_timeout = [ctx, reactor]() {
          if (ctx->read_timer_id != -1) {
            reactor->unregister_timer(ctx->read_timer_id);
            ctx->read_timer_id = -1;
          }
        };

        arm_idle(); // start idle timer before the first byte arrives

        // ── Per-connection read callback ──────────────────────────────────
        reactor->register_event(
            client_fd, EventType::Read,
            [this, reactor, ctx, arm_idle,
             arm_read_timeout, cancel_read_timeout](int cfd) mutable {
              // Activity received — disarm idle timer
              if (ctx->idle_timer_id != -1) {
                reactor->unregister_timer(ctx->idle_timer_id);
                ctx->idle_timer_id = -1;
              }

              // Read available bytes into accumulation buffer.
              // Prefetch ctx fields used immediately after read() into L1 cache.
              __builtin_prefetch(&ctx->buf,        0, 3);
              __builtin_prefetch(&ctx->handled,    0, 1);
              __builtin_prefetch(&ctx->idle_timer_id, 0, 1);
              char tmp[8192]; // NOLINT(modernize-avoid-c-arrays)
              ssize_t n = read(cfd, tmp, sizeof(tmp));
              if (n <= 0) [[unlikely]] {
                cancel_read_timeout();
                reactor->unregister_event(cfd, EventType::Read);
                close(cfd);
                return;
              }

              // Start per-request read timeout on first byte of a new request.
              if (ctx->buf.empty()) [[likely]] arm_read_timeout();

              ctx->buf.append(tmp, static_cast<size_t>(n));

              // Re-parse accumulated buffer from the beginning each time.
              // This is simple and correct for typical request sizes.
              HttpParser parser;
              Request req;
              auto parsed = parser.parse(ctx->buf, req);

              // ── Hard parse error (400 / 413) ──────────────────────────
              if (!parsed) [[unlikely]] {
                cancel_read_timeout();
                int status = parser.error_status() != 0 ? parser.error_status() : 400;
                Response err;
                err.status(status)
                   .header("Connection", "close")
                   .header("Date", cached_http_date())
                   .body(status == 413 ? "Payload Too Large" : "Bad Request");
                write_all(cfd, err.serialize());
                reactor->unregister_event(cfd, EventType::Read);
                close(cfd);
                return;
              }

              // ── Headers done, body still pending → Expect: 100-continue ─
              if (parser.headers_complete() && !parser.is_complete()) {
                if (!ctx->sent_100) {
                  std::string_view expect = req.header("Expect");
                  if (expect == "100-continue") {
                    static constexpr char k100[] = // NOLINT(modernize-avoid-c-arrays)
                        "HTTP/1.1 100 Continue\r\n\r\n";
                    [[maybe_unused]] ssize_t r100 =
                        write(cfd, k100, sizeof(k100) - 1);
                    ctx->sent_100 = true;
                  }
                }
                arm_idle();
                return;
              }

              if (!parser.is_complete()) {
                // Incomplete request — wait for more data
                arm_idle();
                return;
              }

              // Request is complete.  Cancel read timeout and reset state.
              cancel_read_timeout();
              ctx->sent_100 = false;

              // Record request start time for access log duration.
              auto req_start = std::chrono::steady_clock::now();

              // Consume parsed bytes; remainder stays for pipelining
              ctx->buf.erase(0, *parsed);

              ctx->handled++;
              cnt_requests_.fetch_add(1, std::memory_order_relaxed);

              // ── Path traversal guard ────────────────────────────────────
              {
                std::string_view p = req.path();
                // Check for ../ and common percent-encoded variants
                bool bad = (p.find("/../") != std::string_view::npos ||
                            p.find("/..") == p.size() - 3       ||
                            p.find("%2e%2e") != std::string_view::npos ||
                            p.find("%2E%2E") != std::string_view::npos ||
                            p.find("%2e.") != std::string_view::npos  ||
                            p.find(".%2e") != std::string_view::npos);
                if (bad) {
                  Response err;
                  err.status(400).body("Bad Request");
                  err.header("Date", cached_http_date());
                  err.header("Connection", "close");
#ifdef __linux__
                  { int qa = 1; setsockopt(cfd, IPPROTO_TCP, TCP_QUICKACK, &qa, sizeof(qa)); }
#endif
                  write_all(cfd, err.serialize());
                  reactor->unregister_event(cfd, EventType::Read);
                  close(cfd);
                  return;
                }
              }

              // Expose the remote peer IP via Request::remote_addr().
              req.set_remote_addr(ctx->client_ip);

              // ── Keep-alive decision ─────────────────────────────────────
              auto conn_hdr  = req.header("Connection");
              bool keep_alive =
                  (conn_hdr != "close" &&
                   ctx->handled < ConnCtx::MAX_REQUESTS);

              auto close_conn_fn = [reactor, cfd]() {
                reactor->unregister_event(cfd, EventType::Read);
                close(cfd);
              };

              // ── send_response: ETag / Range / Cork / write ───────────────
              // Used by both sync and async paths.  All args by value so it's
              // safe to capture into a coroutine frame.
              auto send_response = [this, cfd, keep_alive, &ctx,
                                    req_start](const Request &rq,
                                               Response &r) mutable {
                auto if_none_match = rq.header("If-None-Match");
                auto resp_etag     = r.get_header("ETag");
                if (!if_none_match.empty() && !resp_etag.empty()) {
                  if (if_none_match == "*" || if_none_match == resp_etag) {
                    Response nm;
                    nm.status(304)
                      .header("ETag",       resp_etag)
                      .header("Date",       cached_http_date())
                      .header("Connection", keep_alive ? "keep-alive" : "close");
#ifdef __linux__
                    { int qa = 1; setsockopt(cfd, IPPROTO_TCP, TCP_QUICKACK, &qa, sizeof(qa)); }
#endif
                    write_all(cfd, nm.serialize());
                    return;
                  }
                }

                auto range_hdr = rq.header("Range");
                if (!range_hdr.empty() && r.status_code() == 200) {
                  std::string_view body = r.get_body();
                  size_t total = body.size();
                  if (range_hdr.substr(0, 6) == "bytes=") {
                    std::string_view spec = range_hdr.substr(6);
                    size_t dash = spec.find('-');
                    if (dash != std::string_view::npos) {
                      size_t first = 0, last = total - 1;
                      bool ok = true;
                      std::string first_str(spec.substr(0, dash));
                      std::string last_str(spec.substr(dash + 1));
                      try {
                        if (!first_str.empty()) first = std::stoul(first_str);
                        if (!last_str.empty())  last  = std::stoul(last_str);
                        else                    last  = total - 1;
                        if (first > last || first >= total) ok = false;
                        if (last >= total) last = total - 1;
                      } catch (...) { ok = false; }
                      if (ok) {
                        r.status(206)
                         .header("Content-Range",
                                 "bytes " + std::to_string(first) + "-" +
                                 std::to_string(last) + "/" +
                                 std::to_string(total))
                         .header("Accept-Ranges", "bytes")
                         .body(std::string(body.substr(first, last - first + 1)));
                      } else {
                        r.status(416)
                         .header("Content-Range", "bytes */" + std::to_string(total))
                         .body("Range Not Satisfiable");
                      }
                    }
                  }
                } else if (range_hdr.empty() && r.status_code() == 200) {
                  r.header("Accept-Ranges", "bytes");
                }

                r.header("Date", cached_http_date());
                r.header("Connection", keep_alive ? "keep-alive" : "close");
                if (keep_alive) {
                  r.header("Keep-Alive",
                            "timeout=30, max=" +
                                std::to_string(ConnCtx::MAX_REQUESTS - ctx->handled));
                }
#ifdef __linux__
                { int cork = 1; setsockopt(cfd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork)); }
#endif
                std::string hdr = r.serialize_header();
                size_t bytes    = hdr.size();
                std::string_view body_view = r.is_chunked() ? r.chunk_buf() : r.get_body();
                if (r.has_sendfile()) {
                  bytes += r.sendfile_size();
                  write_all(cfd, hdr);
                  send_file_body(cfd, r.get_sendfile_path(), r.sendfile_size());
                } else {
                  bytes += body_view.size();
                  writev_response(cfd, hdr, body_view);
                }
                // Append HTTP Trailers after last chunk (chunked encoding only).
                if (r.is_chunked() && r.has_trailers()) {
                  std::string tr = r.encode_trailers();
                  bytes += tr.size();
                  write_all(cfd, tr);
                }
#ifdef __linux__
                { int cork = 0; setsockopt(cfd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork)); }
                { int qa   = 1; setsockopt(cfd, IPPROTO_TCP, TCP_QUICKACK, &qa, sizeof(qa)); }
#endif
                cnt_bytes_sent_.fetch_add(bytes, std::memory_order_relaxed);
                int sc = r.status_code();
                if (sc >= 400) cnt_errors_.fetch_add(1, std::memory_order_relaxed);
                if (structured_logger_ || logger_) {
                  auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - req_start).count();
                  const char *method_str =
                      rq.method() == Method::Get    ? "GET"    :
                      rq.method() == Method::Post   ? "POST"   :
                      rq.method() == Method::Put    ? "PUT"    :
                      rq.method() == Method::Delete ? "DELETE" :
                      rq.method() == Method::Patch  ? "PATCH"  :
                      rq.method() == Method::Head   ? "HEAD"   :
                      rq.method() == Method::Options? "OPTIONS": "UNKNOWN";
                  if (structured_logger_) {
                    StructuredLogRecord rec;
                    rec.method      = method_str;
                    rec.path        = rq.path();
                    rec.status      = sc;
                    rec.duration_us = dur;
                    rec.remote_addr = rq.remote_addr();
                    rec.request_id  = rq.header("X-Request-Id");
                    // Support both B3 tracing and W3C traceparent
                    rec.trace_id    = rq.header("X-B3-TraceId");
                    if (rec.trace_id.empty())
                      rec.trace_id = rq.header("traceparent");
                    structured_logger_(rec);
                  } else if (logger_) {
                    logger_(method_str, rq.path(), sc, dur);
                  }
                }
              };

              // ── sync finalize (wraps send_response) ─────────────────────
              auto finalize = [&](Response &r) { send_response(req, r); };
              auto close_conn = close_conn_fn;

              // ── Route dispatch (shared by both sync and async paths) ─────
              std::unordered_map<std::string, std::string> params;
              auto handler = router_.match(req.method(), req.path(), params);

              bool head_fallback = false;
              if (std::holds_alternative<std::monostate>(handler) &&
                  req.method() == Method::Head) {
                handler = router_.match(Method::Get, req.path(), params);
                head_fallback = !std::holds_alternative<std::monostate>(handler);
              }

              // ── ASYNC PATH: any AsyncMiddleware registered ───────────────
              if (router_.has_async_middlewares()) {
                // 404 / 405 short-circuit before spawning coroutine
                if (std::holds_alternative<std::monostate>(handler)) {
                  Response err;
                  int st = router_.path_exists(req.path()) ? 405 : 404;
                  err.status(st).body(st == 405 ? "Method Not Allowed" : "Not Found");
                  send_response(req, err);
                  if (!keep_alive) close_conn(); else arm_idle();
                  return;
                }
                for (auto const &[k, v] : params) req.set_param(k, v);

                // Build the tail coroutine: runs route handler + sends response.
                // All state captured by value so the frame owns its data.
                auto async_tail = [handler, head_fallback, ka = keep_alive,
                                   fd = cfd, r = reactor, c = ctx,
                                   arm = arm_idle, eh = error_handler_,
                                   sr = send_response](
                    std::shared_ptr<Request>  rqp,
                    std::shared_ptr<Response> rsp) mutable -> Task<void> {
                  if (std::holds_alternative<Handler>(handler)) {
                    try {
                      std::get<Handler>(handler)(*rqp, *rsp);
                      if (head_fallback) rsp->body("");
                    } catch (...) {
                      auto ep = std::current_exception();
                      *rsp = Response{};
                      if (eh) { try { eh(ep, *rqp, *rsp); } catch (...) {} }
                      else {
                        try { std::rethrow_exception(ep); }
                        catch (const std::exception &ex) {
                          std::cerr << "[ERROR] handler: " << ex.what() << "\n";
                        } catch (...) {}
                        rsp->status(500).body("Internal Server Error");
                      }
                    }
                  } else {
                    auto &ah = std::get<AsyncHandler>(handler);
                    try { co_await ah(*rqp, *rsp); }
                    catch (const std::exception &ex) {
                      std::cerr << "[ERROR] async handler: " << ex.what() << "\n";
                      *rsp = Response{}; rsp->status(500).body("Internal Server Error");
                    } catch (...) {
                      std::cerr << "[ERROR] async handler unknown exception\n";
                      *rsp = Response{}; rsp->status(500).body("Internal Server Error");
                    }
                    if (head_fallback) rsp->body("");
                  }
                  sr(*rqp, *rsp);
                  if (!ka) {
                    r->unregister_event(fd, EventType::Read);
                    close(fd);
                  } else {
                    arm();
                  }
                };

                auto rqp = std::make_shared<Request>(std::move(req));
                auto rsp = std::make_shared<Response>();
                auto chain_task = [](
                    const std::vector<AnyMiddleware> *mws,
                    std::shared_ptr<Request>  rqp,
                    std::shared_ptr<Response> rsp,
                    decltype(async_tail) tail) -> Task<void> {
                  co_await run_mw_chain(mws, 0, rqp, rsp, std::move(tail));
                }(&router_.middlewares(), rqp, rsp, std::move(async_tail));
                chain_task.resume();
                chain_task.detach();
                return; // sync callback done; coroutine owns everything
              }

              // ── SYNC FAST PATH ───────────────────────────────────────────
              Response res;
              bool cont = true;
              for (const auto &any_mw : router_.middlewares()) {
                // Safe: has_async_middlewares() is false here.
                if (!std::get<Middleware>(any_mw)(req, res)) {
                  cont = false;
                  break;
                }
              }

              if (!cont) {
                keep_alive = false;
                finalize(res);
                close_conn();
                return;
              }

              if (std::holds_alternative<std::monostate>(handler)) {
                int status = router_.path_exists(req.path()) ? 405 : 404;
                res.status(status).body(status == 405 ? "Method Not Allowed"
                                                       : "Not Found");
                finalize(res);
                if (!keep_alive) close_conn(); else arm_idle();
                return;
              }

              for (auto const &[key, val] : params)
                req.set_param(key, val);

              if (std::holds_alternative<Handler>(handler)) {
                try {
                  std::get<Handler>(handler)(req, res);
                  if (head_fallback) res.body("");
                } catch (...) {
                  auto ep = std::current_exception();
                  res = Response{};
                  if (error_handler_) {
                    try { error_handler_(ep, req, res); } catch (...) {}
                  } else {
                    try { std::rethrow_exception(ep); }
                    catch (const std::exception &ex) {
                      std::cerr << "[ERROR] handler exception: " << ex.what() << "\n";
                    } catch (...) {
                      std::cerr << "[ERROR] handler unknown exception\n";
                    }
                    res.status(500).body("Internal Server Error");
                  }
                }
                finalize(res);
                if (!keep_alive) close_conn(); else arm_idle();

              } else {
                // ── Async handler (no async MWs — lean coroutine) ────────
                auto h   = std::get<AsyncHandler>(handler);
                bool ka  = keep_alive;
                bool hfb = head_fallback;
                auto sr  = send_response;

                auto run_async =
                    [](AsyncHandler ah, Request areq, Response ares,       // NOLINT(performance-unnecessary-value-param)
                       int fd, Reactor *r, [[maybe_unused]] std::shared_ptr<ConnCtx> c, // NOLINT(performance-unnecessary-value-param)
                       bool ka_flag, bool head_fb,
                       std::function<void()> arm, // NOLINT(performance-unnecessary-value-param)
                       decltype(send_response) sr_fn) -> Task<void> { // NOLINT(performance-unnecessary-value-param)
                  try {
                    co_await ah(areq, ares);
                  } catch (const std::exception &ex) {
                    std::cerr << "[ERROR] async handler exception: " << ex.what() << "\n";
                    ares = Response{}; ares.status(500).body("Internal Server Error");
                  } catch (...) {
                    std::cerr << "[ERROR] async handler unknown exception\n";
                    ares = Response{}; ares.status(500).body("Internal Server Error");
                  }
                  if (head_fb) ares.body("");
                  sr_fn(areq, ares);
                  if (!ka_flag) {
                    r->unregister_event(fd, EventType::Read);
                    close(fd);
                  } else {
                    arm();
                  }
                };

                auto task = run_async(h, std::move(req), std::move(res),
                                      cfd, reactor, ctx, ka, hfb, arm_idle, sr);
                (void)task.resume();
                task.detach();
              }
            });
      }); // register_listener_at

    if (!listen_res) {
      for (int sfd : server_fds) ::close(sfd);
      return std::unexpected(listen_res.error());
    }
  } // for each reactor

  // Store listen fds so stop() can close them for drain mode.
  listen_fds_ = server_fds;

  dispatcher_.run();

  // Close any listen fds that stop() didn't already close.
  for (int sfd : listen_fds_) ::close(sfd);
  listen_fds_.clear();
  return {};
}

// ─── App::listen_unix ─────────────────────────────────────────────────────────

Result<void> App::listen_unix(std::string_view path) {
  // ── SIGTERM / SIGINT → graceful shutdown ──────────────────────────────────
  g_app_instance = this;
  std::signal(SIGTERM, on_shutdown_signal);
  std::signal(SIGINT,  on_shutdown_signal);

  // ── Unix domain socket ────────────────────────────────────────────────────
  int server_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_fd == -1)
    return std::unexpected(
        std::make_error_code(std::errc::resource_unavailable_try_again));

  // Remove any stale socket file.
  ::unlink(std::string(path).c_str());

  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path) - 1) {
    close(server_fd);
    return std::unexpected(std::make_error_code(std::errc::filename_too_long));
  }
  std::memcpy(addr.sun_path, path.data(), path.size());
  addr.sun_path[path.size()] = '\0';

  if (::bind(server_fd, reinterpret_cast<struct sockaddr *>(&addr),
             sizeof(addr)) < 0) {
    close(server_fd);
    return std::unexpected(std::make_error_code(std::errc::address_in_use));
  }

  if (::listen(server_fd, SOMAXCONN) < 0) {
    close(server_fd);
    return std::unexpected(std::make_error_code(std::errc::address_in_use));
  }

  std::cout << "Draco WAS listening on unix:" << path << "\n";

  std::string sock_path(path);

  // ── Accept loop (same structure as TCP listen, minus TCP socket options) ──
  auto listen_res = dispatcher_.register_listener(
      server_fd, [this](int lfd) {
        Reactor *reactor = Reactor::current();

        // accept() on AF_UNIX — no sockaddr needed for peer address.
        int client_fd = ::accept(lfd, nullptr, nullptr);
        if (client_fd == -1) [[unlikely]] {
          if (errno != EAGAIN && errno != EWOULDBLOCK)
            std::cerr << "[ERROR] accept(unix): " << strerror(errno) << "\n";
          return;
        }

        ::fcntl(client_fd, F_SETFL, O_NONBLOCK);
#ifdef SO_NOSIGPIPE
        int nosig = 1;
        setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
#endif

        cnt_active_.fetch_add(1, std::memory_order_relaxed);
        auto ctx = std::shared_ptr<ConnCtx>(new ConnCtx{},
            [this](ConnCtx *c) {
              cnt_active_.fetch_sub(1, std::memory_order_relaxed);
              delete c;
            });
        ctx->client_ip = "unix";

        auto arm_idle = [ctx, reactor, client_fd]() {
          if (ctx->idle_timer_id != -1) {
            reactor->unregister_timer(ctx->idle_timer_id);
            ctx->idle_timer_id = -1;
          }
          auto tres = reactor->register_timer(
              ConnCtx::IDLE_TIMEOUT_MS, [reactor, client_fd, ctx](int) {
                ctx->idle_timer_id = -1;
                reactor->unregister_event(client_fd, EventType::Read);
                close(client_fd);
              });
          if (tres) ctx->idle_timer_id = *tres;
        };

        auto arm_read_timeout = [ctx, reactor, client_fd]() {
          if (ctx->read_timer_id != -1) return;
          auto tres = reactor->register_timer(
              ConnCtx::READ_TIMEOUT_MS, [reactor, client_fd, ctx](int) {
                ctx->read_timer_id = -1;
                Response tout;
                tout.status(408).header("Connection", "close").body("Request Timeout");
                write_all(client_fd, tout.serialize());
                reactor->unregister_event(client_fd, EventType::Read);
                close(client_fd);
              });
          if (tres) ctx->read_timer_id = *tres;
        };

        auto cancel_read_timeout = [ctx, reactor]() {
          if (ctx->read_timer_id != -1) {
            reactor->unregister_timer(ctx->read_timer_id);
            ctx->read_timer_id = -1;
          }
        };

        arm_idle();

        reactor->register_event(
            client_fd, EventType::Read,
            [this, reactor, ctx, arm_idle, arm_read_timeout,
             cancel_read_timeout](int cfd) mutable {
              if (ctx->idle_timer_id != -1) {
                reactor->unregister_timer(ctx->idle_timer_id);
                ctx->idle_timer_id = -1;
              }
              char tmp[8192]; // NOLINT(modernize-avoid-c-arrays)
              ssize_t n = read(cfd, tmp, sizeof(tmp));
              if (n <= 0) [[unlikely]] {
                cancel_read_timeout();
                reactor->unregister_event(cfd, EventType::Read);
                close(cfd);
                return;
              }
              if (ctx->buf.empty()) [[likely]] arm_read_timeout();
              ctx->buf.append(tmp, static_cast<size_t>(n));

              HttpParser parser;
              Request req;
              auto parsed = parser.parse(ctx->buf, req);
              if (!parsed) [[unlikely]] {
                cancel_read_timeout();
                Response err;
                err.status(400).header("Connection","close")
                   .header("Date", cached_http_date()).body("Bad Request");
                write_all(cfd, err.serialize());
                reactor->unregister_event(cfd, EventType::Read);
                close(cfd);
                return;
              }
              if (!parser.is_complete()) return;

              cancel_read_timeout();
              ctx->buf.clear();
              ctx->handled++;

              req.set_remote_addr(ctx->client_ip);
              cnt_requests_.fetch_add(1, std::memory_order_relaxed);

              bool keep_alive = ctx->handled < ConnCtx::MAX_REQUESTS;
              Response res;
              bool cont = true;
              for (const auto &mw : router_.middlewares()) {
                if (std::holds_alternative<Middleware>(mw)) {
                  if (!std::get<Middleware>(mw)(req, res)) {
                    cont = false;
                    break;
                  }
                }
              }

              if (!cont) {
                res.header("Date", cached_http_date())
                   .header("Connection", "close");
                std::string hdr = res.serialize_header();
                writev_response(cfd, hdr, res.get_body());
                reactor->unregister_event(cfd, EventType::Read);
                close(cfd);
                return;
              }

              std::unordered_map<std::string, std::string> params;
              auto handler = router_.match(req.method(), req.path(), params);
              if (std::holds_alternative<std::monostate>(handler)) {
                int sc = router_.path_exists(req.path()) ? 405 : 404;
                res.status(sc).body(sc == 405 ? "Method Not Allowed" : "Not Found");
              } else {
                for (auto const &[key, val] : params)
                  req.set_param(key, val);
                try {
                  if (std::holds_alternative<Handler>(handler))
                    std::get<Handler>(handler)(req, res);
                } catch (...) {
                  res.status(500).body("Internal Server Error");
                }
              }

              res.header("Date", cached_http_date())
                 .header("Connection", keep_alive ? "keep-alive" : "close");
              {
                std::string hdr = res.serialize_header();
                std::string_view bv = res.is_chunked() ? res.chunk_buf() : res.get_body();
                writev_response(cfd, hdr, bv);
              }
              cnt_bytes_sent_.fetch_add(1, std::memory_order_relaxed);
              int sc = res.status_code();
              if (sc >= 400) cnt_errors_.fetch_add(1, std::memory_order_relaxed);

              if (!keep_alive) {
                reactor->unregister_event(cfd, EventType::Read);
                close(cfd);
              } else {
                arm_idle();
              }
            });
      });
  if (!listen_res)
    return std::unexpected(listen_res.error());

  dispatcher_.run();

  close(server_fd);
  ::unlink(sock_path.c_str());
  return {};
}

// ─── StackController ──────────────────────────────────────────────────────────

void StackController::add(App &app, int port, bool ipv6) {
  entries_.push_back({&app, port, ipv6});
}

void StackController::stop() {
  if (stopping_.exchange(true, std::memory_order_relaxed))
    return;
  for (auto &e : entries_)
    e.app->stop();
}

void StackController::run() {
  // Install signal handlers — delegate to stop().
  static StackController *g_ctrl = nullptr;
  g_ctrl = this;
  auto sig_handler = [](int) { if (g_ctrl != nullptr) g_ctrl->stop(); };
  std::signal(SIGTERM, sig_handler);
  std::signal(SIGINT,  sig_handler);

  // Start each app in its own thread.
  std::vector<std::jthread> threads;
  threads.reserve(entries_.size());
  for (auto &e : entries_) {
    threads.emplace_back([&e]() {
      if (auto r = e.app->listen(e.port, e.ipv6); !r) {
        std::cerr << "[StackController] listen(" << e.port
                  << ") failed: " << r.error().message() << "\n";
      }
    });
  }
  // std::jthread auto-joins on destruction.
}

} // namespace qbuem
