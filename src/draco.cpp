#include <draco/draco.hpp>
#include <draco/http/parser.hpp>

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <variant>

namespace draco {

// ─── Date header cache (RFC 7231 §7.1.1.2) ──────────────────────────────────
// Refreshed at most once per second; lock held only on update (rare path).
namespace {

std::atomic<std::time_t> g_date_ts{0};
char                      g_date_buf[48] = {};
std::mutex                g_date_mu;

std::string_view cached_http_date() noexcept {
  std::time_t now = std::time(nullptr);
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

// ─── Graceful shutdown ───────────────────────────────────────────────────────
static App *g_app_instance = nullptr;

static void on_shutdown_signal(int) {
  if (g_app_instance)
    g_app_instance->stop();
}

// ─── Per-connection keep-alive state ────────────────────────────────────────
struct ConnCtx {
  std::string buf;          // partial-read accumulation buffer
  int handled       = 0;   // requests handled on this connection
  int idle_timer_id = -1;  // current idle timer (-1 = none)

  static constexpr int MAX_REQUESTS    = 100;
  static constexpr int IDLE_TIMEOUT_MS = 30'000; // 30 s
};

} // namespace

// ─── App ─────────────────────────────────────────────────────────────────────
App::App(size_t thread_count) : dispatcher_(thread_count) {
  std::cout << "Draco WAS v" << Version::string << " initializing ("
            << thread_count << " reactor threads)..." << std::endl;
}

void App::use(Middleware mw) { router_.use(std::move(mw)); }

void App::get(std::string_view path, HandlerVariant handler) {
  router_.add_route(Method::Get, path, std::move(handler));
}

void App::post(std::string_view path, HandlerVariant handler) {
  router_.add_route(Method::Post, path, std::move(handler));
}

void App::stop() { dispatcher_.stop(); }

Result<void> App::listen(int port) {
  // ── SIGTERM / SIGINT → graceful shutdown ──────────────────────────────────
  g_app_instance = this;
  std::signal(SIGTERM, on_shutdown_signal);
  std::signal(SIGINT,  on_shutdown_signal);

  // ── Listening socket ──────────────────────────────────────────────────────
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd == -1)
    return std::unexpected(
        std::make_error_code(std::errc::resource_unavailable_try_again));

  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in address{};
  address.sin_family      = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port        = htons(static_cast<uint16_t>(port));

  if (bind(server_fd, reinterpret_cast<struct sockaddr *>(&address),
           sizeof(address)) < 0) {
    close(server_fd);
    return std::unexpected(std::make_error_code(std::errc::address_in_use));
  }

  if (::listen(server_fd, 1024) < 0) {
    close(server_fd);
    return std::unexpected(std::make_error_code(std::errc::address_in_use));
  }

  fcntl(server_fd, F_SETFL, O_NONBLOCK);
  std::cout << "Draco WAS v" << Version::string
            << " listening on http://0.0.0.0:" << port << std::endl;

  // ── Accept loop ───────────────────────────────────────────────────────────
  auto listen_res = dispatcher_.register_listener(
      server_fd, [this](int lfd) {
        Reactor *reactor = Reactor::current();

        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd =
            accept(lfd, reinterpret_cast<struct sockaddr *>(&client_addr),
                   &client_len);
        if (client_fd == -1) {
          if (errno != EAGAIN && errno != EWOULDBLOCK)
            std::cerr << "[ERROR] accept: " << strerror(errno) << std::endl;
          return;
        }

        fcntl(client_fd, F_SETFL, O_NONBLOCK);

        // Per-connection shared state (lives as long as any callback holds it)
        auto ctx = std::make_shared<ConnCtx>();

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

        arm_idle(); // start idle timer before the first byte arrives

        // ── Per-connection read callback ──────────────────────────────────
        reactor->register_event(
            client_fd, EventType::Read,
            [this, reactor, ctx, arm_idle](int cfd) mutable {
              // Activity received — disarm idle timer
              if (ctx->idle_timer_id != -1) {
                reactor->unregister_timer(ctx->idle_timer_id);
                ctx->idle_timer_id = -1;
              }

              // Read available bytes into accumulation buffer
              char tmp[8192];
              ssize_t n = read(cfd, tmp, sizeof(tmp));
              if (n <= 0) {
                reactor->unregister_event(cfd, EventType::Read);
                close(cfd);
                return;
              }
              ctx->buf.append(tmp, static_cast<size_t>(n));

              // Attempt to parse one complete HTTP request from the buffer
              HttpParser parser;
              Request req;
              auto parsed = parser.parse(ctx->buf, req);

              if (!parser.is_complete()) {
                // Incomplete request — wait for more data
                arm_idle();
                return;
              }

              // Consume parsed bytes; remainder stays for pipelining
              if (parsed)
                ctx->buf.erase(0, *parsed);

              ctx->handled++;

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
                r.header("Date", cached_http_date());
                r.header("Connection", keep_alive ? "keep-alive" : "close");
                if (keep_alive) {
                  r.header("Keep-Alive",
                            "timeout=30, max=" +
                                std::to_string(ConnCtx::MAX_REQUESTS -
                                               ctx->handled));
                }
                write_all(cfd, r.serialize());
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

              if (std::holds_alternative<std::monostate>(handler)) {
                res.status(404).body("Not Found");
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
                std::get<Handler>(handler)(req, res);
                finalize(res);
                if (!keep_alive)
                  close_conn();
                else
                  arm_idle();

              } else {
                // ── Async handler (coroutine) ─────────────────────────────
                // Capture everything by value into the coroutine frame so
                // all state is valid for the lifetime of the co_await chain.
                auto h  = std::get<AsyncHandler>(handler);
                bool ka = keep_alive;

                auto run_async =
                    [](AsyncHandler ah, Request areq, Response ares,
                       int fd, Reactor *r, std::shared_ptr<ConnCtx> c,
                       bool ka_flag,
                       std::function<void()> arm) -> Task<void> {
                  co_await ah(areq, ares);

                  ares.header("Date", cached_http_date());
                  ares.header("Connection",
                               ka_flag ? "keep-alive" : "close");
                  if (ka_flag) {
                    ares.header("Keep-Alive",
                                 "timeout=30, max=" +
                                     std::to_string(ConnCtx::MAX_REQUESTS -
                                                    c->handled));
                  }
                  write_all(fd, ares.serialize());

                  if (!ka_flag) {
                    r->unregister_event(fd, EventType::Read);
                    close(fd);
                  } else {
                    arm(); // restart idle timer for next request
                  }
                };

                auto task = run_async(h, std::move(req), std::move(res),
                                      cfd, reactor, ctx, ka, arm_idle);
                task.resume();
                task.detach(); // frame self-destructs on completion
              }
            });
      });

  if (!listen_res)
    return std::unexpected(listen_res.error());

  dispatcher_.run();

  close(server_fd);
  return {};
}

} // namespace draco
