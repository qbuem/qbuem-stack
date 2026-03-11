#pragma once

#include <draco/common.hpp>
#include <draco/core/dispatcher.hpp>
#include <draco/http/router.hpp>
#include <draco/version.hpp>

#include <string_view>
#include <thread>

namespace draco {

/**
 * @brief Main application class for Draco WAS.
 */
class App {
public:
  /**
   * @brief Construct a new App object.
   * @param thread_count Number of reactor threads. Defaults to hardware
   * concurrency.
   */
  explicit App(size_t thread_count = std::thread::hardware_concurrency());

  /** @brief Register a global middleware. */
  void use(Middleware mw);

  /** @brief Register a GET route. */
  void get(std::string_view path, HandlerVariant handler);

  /** @brief Register a POST route. */
  void post(std::string_view path, HandlerVariant handler);

  /** @brief Register a PUT route. */
  void put(std::string_view path, HandlerVariant handler);

  /**
   * @brief Register a DELETE route.
   * Named del() to avoid collision with the C keyword \c delete.
   */
  void del(std::string_view path, HandlerVariant handler);

  /** @brief Register a PATCH route. */
  void patch(std::string_view path, HandlerVariant handler);

  /**
   * @brief Register a HEAD route.
   *
   * If no explicit HEAD handler is registered but a GET handler exists for the
   * same path, Draco automatically serves HEAD using the GET handler and strips
   * the response body before sending.
   */
  void head(std::string_view path, HandlerVariant handler);

  /**
   * @brief Register an OPTIONS route (CORS preflight etc.).
   */
  void options(std::string_view path, HandlerVariant handler);

  /**
   * @brief Start listening on a port (blocks until stop() is called).
   *
   * SIGTERM and SIGINT are handled automatically: they trigger a graceful
   * drain — no new connections are accepted, and the server exits after the
   * current poll cycle finishes (≤100 ms).
   */
  Result<void> listen(int port);

  /**
   * @brief Request graceful shutdown.
   *
   * Safe to call from a signal handler (async-signal-safe: sets an atomic).
   * listen() will return after the next reactor poll cycle.
   */
  void stop();

private:
  Dispatcher dispatcher_;
  Router router_;
};

} // namespace draco
