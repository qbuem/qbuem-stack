#include <draco/draco.hpp>
#include <draco/http/parser.hpp>

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <variant>

namespace draco {

namespace {

// Best-effort write: loop until all bytes are sent or an unrecoverable error.
void write_all(int fd, const std::string &data) {
  const char *ptr = data.data();
  ssize_t remaining = static_cast<ssize_t>(data.size());
  while (remaining > 0) {
    ssize_t n = write(fd, ptr, static_cast<size_t>(remaining));
    if (n <= 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    ptr += n;
    remaining -= n;
  }
}

} // namespace

App::App(size_t thread_count) : dispatcher_(thread_count) {
  std::cout << "Draco WAS v" << Version::string << " Initializing..."
            << std::endl;
}

void App::use(Middleware mw) { router_.use(std::move(mw)); }

void App::get(std::string_view path, HandlerVariant handler) {
  router_.add_route(Method::Get, path, std::move(handler));
}

void App::post(std::string_view path, HandlerVariant handler) {
  router_.add_route(Method::Post, path, std::move(handler));
}

Result<void> App::listen(int port) {
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd == -1) {
    return std::unexpected(
        std::make_error_code(std::errc::resource_unavailable_try_again));
  }

  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(static_cast<uint16_t>(port));

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

  auto listen_res =
      dispatcher_.register_listener(server_fd, [this](int lfd) {
        // This callback runs in reactor 0's thread.
        // We use Reactor::current() so that client fd registration stays on
        // the same reactor, avoiding cross-thread access to fd_callbacks_.
        Reactor *reactor = Reactor::current();

        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd =
            accept(lfd, reinterpret_cast<struct sockaddr *>(&client_addr),
                   &client_len);
        if (client_fd == -1) {
          if (errno != EAGAIN && errno != EWOULDBLOCK) {
            std::cerr << "[ERROR] accept: " << strerror(errno) << std::endl;
          }
          return;
        }

        fcntl(client_fd, F_SETFL, O_NONBLOCK);

        // Register client fd on the SAME reactor (same thread) to avoid
        // cross-thread races on fd_callbacks_. True load-balancing across
        // reactors requires SO_REUSEPORT and a separate listener per core.
        reactor->register_event(
            client_fd, EventType::Read,
            [this, reactor](int cfd) {
              char buffer[8192];
              ssize_t n = read(cfd, buffer, sizeof(buffer));
              if (n <= 0) {
                // Always unregister before close to keep fd_callbacks_ clean.
                // Without this, fd reuse causes EPOLL_CTL_MOD to fail on the
                // recycled fd number → connection hangs.
                reactor->unregister_event(cfd, EventType::Read);
                close(cfd);
                return;
              }

              std::string_view data(buffer, static_cast<size_t>(n));
              Request req;
              HttpParser parser;
              auto parsed_bytes = parser.parse(data, req);

              if (!parsed_bytes) {
                std::cerr << "[ERROR] HTTP parse error" << std::endl;
                reactor->unregister_event(cfd, EventType::Read);
                close(cfd);
                return;
              }

              if (!parser.is_complete())
                return; // Wait for more data.

              Response res;
              bool cont = true;
              for (const auto &mw : router_.middlewares()) {
                if (!mw(req, res)) {
                  cont = false;
                  break;
                }
              }

              if (!cont) {
                reactor->unregister_event(cfd, EventType::Read);
                write_all(cfd, res.serialize());
                close(cfd);
                return;
              }

              std::unordered_map<std::string, std::string> params;
              auto handler = router_.match(req.method(), req.path(), params);

              if (std::holds_alternative<std::monostate>(handler)) {
                res.status(404).body("Not Found");
                reactor->unregister_event(cfd, EventType::Read);
                write_all(cfd, res.serialize());
                close(cfd);
                return;
              }

              for (auto const &[key, val] : params)
                req.set_param(key, val);

              if (std::holds_alternative<Handler>(handler)) {
                std::get<Handler>(handler)(req, res);
                reactor->unregister_event(cfd, EventType::Read);
                write_all(cfd, res.serialize());
                close(cfd);
              } else {
                // Async handler: launch as a self-managing detached coroutine.
                // The coroutine owns the fd lifecycle; it must unregister
                // before closing.
                auto h = std::get<AsyncHandler>(handler);
                auto run_async = [](AsyncHandler ah, Request areq,
                                    Response ares, int fd,
                                    Reactor *r) -> Task<void> {
                  co_await ah(areq, ares);
                  r->unregister_event(fd, EventType::Read);
                  write_all(fd, ares.serialize());
                  close(fd);
                };
                auto task =
                    run_async(h, std::move(req), std::move(res), cfd, reactor);
                task.resume();
                task.detach(); // Frame self-destructs on completion.
              }
            });
      });

  if (!listen_res)
    return std::unexpected(listen_res.error());

  dispatcher_.run();
  return {};
}

} // namespace draco
