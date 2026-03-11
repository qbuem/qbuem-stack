#include <draco/draco.hpp>
#include <draco/http/parser.hpp>
#include <draco/middleware/static_files.hpp>

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
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
  std::string buf;           // partial-read accumulation buffer
  int handled        = 0;   // requests handled on this connection
  int idle_timer_id  = -1;  // current idle timer (-1 = none)
  int read_timer_id  = -1;  // per-request read timeout timer (-1 = none)
  bool sent_100      = false; // already sent 100 Continue for current request

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
#ifdef SO_REUSEPORT
  // Allow the kernel to load-balance incoming connections across multiple
  // listener sockets (one per reactor thread in future multi-socket mode).
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

#ifdef __linux__
  // TCP_DEFER_ACCEPT: only wake up accept() once data has arrived.
  // Eliminates the accept → read → empty-read cycle for slow clients.
  int defer_secs = 5;
  setsockopt(server_fd, IPPROTO_TCP, TCP_DEFER_ACCEPT,
             &defer_secs, sizeof(defer_secs));
#endif

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
#endif

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

              // Read available bytes into accumulation buffer
              char tmp[8192];
              ssize_t n = read(cfd, tmp, sizeof(tmp));
              if (n <= 0) {
                cancel_read_timeout();
                reactor->unregister_event(cfd, EventType::Read);
                close(cfd);
                return;
              }

              // Start per-request read timeout on first byte of a new request.
              if (ctx->buf.empty()) arm_read_timeout();

              ctx->buf.append(tmp, static_cast<size_t>(n));

              // Re-parse accumulated buffer from the beginning each time.
              // This is simple and correct for typical request sizes.
              HttpParser parser;
              Request req;
              auto parsed = parser.parse(ctx->buf, req);

              // ── Hard parse error (400 / 413) ──────────────────────────
              if (!parsed) {
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

              // Consume parsed bytes; remainder stays for pipelining
              ctx->buf.erase(0, *parsed);

              ctx->handled++;

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
                // Re-enable TCP_QUICKACK after each response write.
                int qa = 1;
                setsockopt(cfd, IPPROTO_TCP, TCP_QUICKACK, &qa, sizeof(qa));
#endif
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
                } catch (const std::exception &ex) {
                  std::cerr << "[ERROR] handler exception: " << ex.what()
                            << std::endl;
                  res = Response{};
                  res.status(500).body("Internal Server Error");
                } catch (...) {
                  std::cerr << "[ERROR] handler unknown exception" << std::endl;
                  res = Response{};
                  res.status(500).body("Internal Server Error");
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
                  int qa = 1;
                  setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &qa, sizeof(qa));
#endif
                  write_all(fd, ares.serialize());

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
    return std::unexpected(listen_res.error());

  dispatcher_.run();

  close(server_fd);
  return {};
}

} // namespace draco
