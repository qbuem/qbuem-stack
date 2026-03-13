#include <qbuem/qbuem-stack.hpp>
#include <qbuem/http/parser.hpp>
#include <qbuem/middleware/static_files.hpp>

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <ctime>
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
#ifdef __linux__
#  include <fcntl.h>
#  include <sys/sendfile.h>
#endif

namespace qbuem {

// ─── Date header cache (RFC 7231 §7.1.1.2) ──────────────────────────────────
// Refreshed at most once per second; lock held only on update (rare path).
namespace {

std::atomic<std::time_t> g_date_ts{0};
char                      g_date_buf[48] = {};
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
  struct iovec iov[2];
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
  if (g_app_instance)
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

} // namespace

// ─── App ─────────────────────────────────────────────────────────────────────
App::App(size_t thread_count) : dispatcher_(thread_count) {
  std::cout << "Draco WAS v" << Version::string << " initializing ("
            << thread_count << " reactor threads)..." << std::endl;
}

void App::use(Middleware mw) { router_.use(std::move(mw)); }

void App::on_error(ErrorHandler handler) { error_handler_ = std::move(handler); }

void App::get(std::string_view path, HandlerVariant handler) {
  router_.add_route(Method::Get, path, std::move(handler));
}
void App::post(std::string_view path, HandlerVariant handler) {
  router_.add_route(Method::Post, path, std::move(handler));
}
void App::put(std::string_view path, HandlerVariant handler) {
  router_.add_route(Method::Put, path, std::move(handler));
}
void App::del(std::string_view path, HandlerVariant handler) {
  router_.add_route(Method::Delete, path, std::move(handler));
}
void App::patch(std::string_view path, HandlerVariant handler) {
  router_.add_route(Method::Patch, path, std::move(handler));
}
void App::head(std::string_view path, HandlerVariant handler) {
  router_.add_route(Method::Head, path, std::move(handler));
}
void App::options(std::string_view path, HandlerVariant handler) {
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

void App::enable_access_log() {
  logger_ = [](std::string_view method, std::string_view path, int status,
               long duration_us) {
    // Format: [YYYY-MM-DDTHH:MM:SSZ] METHOD /path STATUS Xµs
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);
    std::fprintf(stderr, "[%s] %.*s %.*s %d %ldµs\n", ts,
                 static_cast<int>(method.size()), method.data(),
                 static_cast<int>(path.size()),   path.data(),
                 status, duration_us);
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
    std::fprintf(stderr,
                 "{\"ts\":\"%s\",\"method\":\"%.*s\","
                 "\"path\":\"%.*s\",\"status\":%d,\"duration_us\":%ld}\n",
                 ts,
                 static_cast<int>(method.size()), method.data(),
                 static_cast<int>(path.size()),   path.data(),
                 status, duration_us);
  };
}

void App::set_max_connections(uint64_t max) {
  max_connections_.store(max, std::memory_order_relaxed);
}

// ─── App lifecycle ────────────────────────────────────────────────────────────

void App::stop() {
  draining_.store(true, std::memory_order_relaxed);
  dispatcher_.stop();
}

Result<void> App::listen(int port, bool ipv6) {
  start_time_ = std::chrono::steady_clock::now();
  draining_.store(false, std::memory_order_relaxed);
  // ── SIGTERM / SIGINT → graceful shutdown ──────────────────────────────────
  g_app_instance = this;
  std::signal(SIGTERM, on_shutdown_signal);
  std::signal(SIGINT,  on_shutdown_signal);

  // ── Listening socket ──────────────────────────────────────────────────────
  int af = ipv6 ? AF_INET6 : AF_INET;
  int server_fd = socket(af, SOCK_STREAM, 0);
  if (server_fd == -1)
    return unexpected(
        std::make_error_code(std::errc::resource_unavailable_try_again));

  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
  // Allow the kernel to load-balance incoming connections across multiple
  // listener sockets (one per reactor thread in future multi-socket mode).
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

#if defined(__linux__) && defined(TCP_FASTOPEN)
  // TCP Fast Open: allow data in the SYN packet for repeat clients,
  // reducing connection latency by one RTT.
  int tfo_qlen = 128;
  setsockopt(server_fd, IPPROTO_TCP, TCP_FASTOPEN, &tfo_qlen, sizeof(tfo_qlen));
#endif

  if (ipv6) {
    // IPV6_V6ONLY = 0: accept both IPv4 and IPv6 (dual-stack).
    int v6only = 0;
    setsockopt(server_fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
  }

#ifdef __linux__
  // TCP_DEFER_ACCEPT: only wake up accept() once data has arrived.
  // Eliminates the accept → read → empty-read cycle for slow clients.
  int defer_secs = 5;
  setsockopt(server_fd, IPPROTO_TCP, TCP_DEFER_ACCEPT,
             &defer_secs, sizeof(defer_secs));
#endif

  // Bind to INADDR_ANY / in6addr_any
  if (ipv6) {
    struct sockaddr_in6 addr6{};
    addr6.sin6_family = AF_INET6;
    addr6.sin6_addr   = in6addr_any;
    addr6.sin6_port   = htons(static_cast<uint16_t>(port));
    if (bind(server_fd, reinterpret_cast<struct sockaddr *>(&addr6),
             sizeof(addr6)) < 0) {
      close(server_fd);
      return unexpected(std::make_error_code(std::errc::address_in_use));
    }
  } else {
    struct sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port        = htons(static_cast<uint16_t>(port));
    if (bind(server_fd, reinterpret_cast<struct sockaddr *>(&address),
             sizeof(address)) < 0) {
      close(server_fd);
      return unexpected(std::make_error_code(std::errc::address_in_use));
    }
  }

  if (::listen(server_fd, 1024) < 0) {
    close(server_fd);
    return unexpected(std::make_error_code(std::errc::address_in_use));
  }

  fcntl(server_fd, F_SETFL, O_NONBLOCK);
  std::cout << "Draco WAS v" << Version::string
            << " listening on http" << (ipv6 ? "://[::]:" : "://0.0.0.0:")
            << port << std::endl;

  // ── Accept loop ───────────────────────────────────────────────────────────
  auto listen_res = dispatcher_.register_listener(
      server_fd, [this](int lfd) {
        Reactor *reactor = Reactor::current();

        struct sockaddr_storage client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd =
            accept(lfd, reinterpret_cast<struct sockaddr *>(&client_addr),
                   &client_len);
        if (client_fd == -1) {
          if (errno != EAGAIN && errno != EWOULDBLOCK)
            std::cerr << "[ERROR] accept: " << strerror(errno) << std::endl;
          return;
        }

        // Extract remote peer IP address for use in Request::remote_addr().
        char peer_ip[INET6_ADDRSTRLEN] = {};
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
              char tmp[8192];
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
                int status = parser.error_status() ? parser.error_status() : 400;
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
                    static constexpr char k100[] =
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

              // ── Middleware chain ────────────────────────────────────────
              Response res;
              bool cont = true;
              for (const auto &mw : router_.middlewares()) {
                if (!mw(req, res)) {
                  cont = false;
                  break;
                }
              }

              // ── Keep-alive decision ─────────────────────────────────────
              // HTTP/1.1 defaults to keep-alive; honour explicit "close".
              auto conn_hdr  = req.header("Connection");
              bool keep_alive =
                  (conn_hdr != "close" &&
                   ctx->handled < ConnCtx::MAX_REQUESTS);

              // ── Helpers ─────────────────────────────────────────────────
              auto finalize = [&](Response &r) {
                // ── Conditional request: If-None-Match / ETag ───────────
                // If the handler set an ETag and the client already holds a
                // matching version, respond 304 Not Modified (no body).
                auto if_none_match = req.header("If-None-Match");
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

                // ── Range Request (RFC 7233) ────────────────────────────
                // Only handle byte ranges on 200 OK responses with a body.
                auto range_hdr = req.header("Range");
                if (!range_hdr.empty() && r.status_code() == 200) {
                  std::string_view body = r.get_body();
                  size_t total = body.size();
                  // Parse "bytes=N-M" (simple single-range, no multi-range)
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
                        std::string slice(body.substr(first, last - first + 1));
                        r.status(206)
                         .header("Content-Range",
                                 "bytes " + std::to_string(first) + "-" +
                                 std::to_string(last) + "/" +
                                 std::to_string(total))
                         .header("Accept-Ranges", "bytes")
                         .body(slice);
                      } else {
                        // Unsatisfiable range → 416
                        r.status(416)
                         .header("Content-Range",
                                 "bytes */" + std::to_string(total))
                         .body("Range Not Satisfiable");
                      }
                    }
                  }
                } else if (range_hdr.empty() && r.status_code() == 200) {
                  // Advertise range support on normal responses
                  r.header("Accept-Ranges", "bytes");
                }

                r.header("Date", cached_http_date());
                r.header("Connection", keep_alive ? "keep-alive" : "close");
                if (keep_alive) {
                  r.header("Keep-Alive",
                            "timeout=30, max=" +
                                std::to_string(ConnCtx::MAX_REQUESTS -
                                               ctx->handled));
                }
#ifdef __linux__
                // TCP_CORK: delay segment until uncorked, ensuring header+body
                // go in one TCP segment.  TCP_QUICKACK re-enabled after send.
                { int cork = 1; setsockopt(cfd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork)); }
#endif
                std::string hdr = r.serialize_header();
                size_t bytes = hdr.size();
                std::string_view body_view =
                    r.is_chunked() ? r.chunk_buf() : r.get_body();
#ifdef __linux__
                if (r.has_sendfile()) {
                  // Zero-copy: write header, then sendfile() body.
                  bytes += r.sendfile_size();
                  write_all(cfd, hdr);
                  send_file_body(cfd, r.get_sendfile_path(), r.sendfile_size());
                } else {
                  bytes += body_view.size();
                  writev_response(cfd, hdr, body_view);
                }
                { int cork = 0; setsockopt(cfd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork)); }
                { int qa = 1; setsockopt(cfd, IPPROTO_TCP, TCP_QUICKACK, &qa, sizeof(qa)); }
#else
                {
                  bytes += body_view.size();
                  writev_response(cfd, hdr, body_view);
                }
#endif

                // ── Metrics + access log ────────────────────────────────
                cnt_bytes_sent_.fetch_add(bytes, std::memory_order_relaxed);
                int sc = r.status_code();
                if (sc >= 400)
                  cnt_errors_.fetch_add(1, std::memory_order_relaxed);
                if (logger_) {
                  auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - req_start).count();
                  logger_(req.method() == Method::Get    ? "GET"    :
                          req.method() == Method::Post   ? "POST"   :
                          req.method() == Method::Put    ? "PUT"    :
                          req.method() == Method::Delete ? "DELETE" :
                          req.method() == Method::Patch  ? "PATCH"  :
                          req.method() == Method::Head   ? "HEAD"   :
                          req.method() == Method::Options? "OPTIONS": "UNKNOWN",
                          req.path(), sc, dur);
                }
              };

              auto close_conn = [&]() {
                reactor->unregister_event(cfd, EventType::Read);
                close(cfd);
              };

              if (!cont) {
                keep_alive = false;
                finalize(res);
                close_conn();
                return;
              }

              // ── Route dispatch ──────────────────────────────────────────
              std::unordered_map<std::string, std::string> params;
              auto handler =
                  router_.match(req.method(), req.path(), params);

              // HEAD fallback: if no explicit HEAD handler, try GET handler and
              // strip the response body before sending.
              bool head_fallback = false;
              if (std::holds_alternative<std::monostate>(handler) &&
                  req.method() == Method::Head) {
                handler = router_.match(Method::Get, req.path(), params);
                head_fallback = !std::holds_alternative<std::monostate>(handler);
              }

              if (std::holds_alternative<std::monostate>(handler)) {
                // Distinguish 404 (path unknown) from 405 (wrong method)
                int status = router_.path_exists(req.path()) ? 405 : 404;
                res.status(status).body(status == 405 ? "Method Not Allowed"
                                                       : "Not Found");
                finalize(res);
                if (!keep_alive)
                  close_conn();
                else
                  arm_idle();
                return;
              }

              for (auto const &[key, val] : params)
                req.set_param(key, val);

              if (std::holds_alternative<Handler>(handler)) {
                // ── Sync handler ──────────────────────────────────────────
                // ── Panic recovery ────────────────────────────────────────
                try {
                  std::get<Handler>(handler)(req, res);
                  if (head_fallback) res.body(""); // HEAD: suppress body
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
                if (!keep_alive)
                  close_conn();
                else
                  arm_idle();

              } else {
                // ── Async handler (coroutine) ─────────────────────────────
                // Capture everything by value into the coroutine frame so
                // all state is valid for the lifetime of the co_await chain.
                auto h   = std::get<AsyncHandler>(handler);
                bool ka  = keep_alive;
                bool hfb = head_fallback;

                auto run_async =
                    [](AsyncHandler ah, Request areq, Response ares,
                       int fd, Reactor *r, std::shared_ptr<ConnCtx> c,
                       bool ka_flag, bool head_fb,
                       std::function<void()> arm) -> Task<void> {
                  try {
                    co_await ah(areq, ares);
                  } catch (const std::exception &ex) {
                    std::cerr << "[ERROR] async handler exception: " << ex.what()
                              << std::endl;
                    ares = Response{};
                    ares.status(500).body("Internal Server Error");
                  } catch (...) {
                    std::cerr << "[ERROR] async handler unknown exception"
                              << std::endl;
                    ares = Response{};
                    ares.status(500).body("Internal Server Error");
                  }
                  if (head_fb) ares.body(""); // HEAD: suppress body

                  ares.header("Date", cached_http_date());
                  ares.header("Connection",
                               ka_flag ? "keep-alive" : "close");
                  if (ka_flag) {
                    ares.header("Keep-Alive",
                                 "timeout=30, max=" +
                                     std::to_string(ConnCtx::MAX_REQUESTS -
                                                    c->handled));
                  }
#ifdef __linux__
                  { int cork = 1; setsockopt(fd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork)); }
#endif
                  {
                    std::string ahdr = ares.serialize_header();
                    writev_response(fd, ahdr, ares.get_body());
                  }
#ifdef __linux__
                  { int cork = 0; setsockopt(fd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork)); }
                  { int qa = 1; setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &qa, sizeof(qa)); }
#endif

                  if (!ka_flag) {
                    r->unregister_event(fd, EventType::Read);
                    close(fd);
                  } else {
                    arm(); // restart idle timer for next request
                  }
                };

                auto task = run_async(h, std::move(req), std::move(res),
                                      cfd, reactor, ctx, ka, hfb, arm_idle);
                task.resume();
                task.detach(); // frame self-destructs on completion
              }
            });
      });

  if (!listen_res)
    return unexpected(listen_res.error());

  dispatcher_.run();

  close(server_fd);
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
    return unexpected(
        std::make_error_code(std::errc::resource_unavailable_try_again));

  // Remove any stale socket file.
  ::unlink(std::string(path).c_str());

  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path) - 1) {
    close(server_fd);
    return unexpected(std::make_error_code(std::errc::filename_too_long));
  }
  std::memcpy(addr.sun_path, path.data(), path.size());
  addr.sun_path[path.size()] = '\0';

  if (::bind(server_fd, reinterpret_cast<struct sockaddr *>(&addr),
             sizeof(addr)) < 0) {
    close(server_fd);
    return unexpected(std::make_error_code(std::errc::address_in_use));
  }

  if (::listen(server_fd, SOMAXCONN) < 0) {
    close(server_fd);
    return unexpected(std::make_error_code(std::errc::address_in_use));
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
              char tmp[8192];
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
                if (!mw(req, res)) {
                  cont = false;
                  break;
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
    return unexpected(listen_res.error());

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
  auto sig_handler = [](int) { if (g_ctrl) g_ctrl->stop(); };
  std::signal(SIGTERM, sig_handler);
  std::signal(SIGINT,  sig_handler);

  // Start each app in its own thread.
  std::vector<std::thread> threads;
  threads.reserve(entries_.size());
  for (auto &e : entries_) {
    threads.emplace_back([&e]() {
      if (auto r = e.app->listen(e.port, e.ipv6); !r) {
        std::cerr << "[StackController] listen(" << e.port
                  << ") failed: " << r.error().message() << "\n";
      }
    });
  }

  for (auto &t : threads)
    if (t.joinable()) t.join();
}

} // namespace qbuem
