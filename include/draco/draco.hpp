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

  /**
   * @brief Register a global middleware.
   */
  void use(Middleware mw);

  /**
   * @brief Register a GET route.
   */
  void get(std::string_view path, HandlerVariant handler);

  /**
   * @brief Register a POST route.
   */
  void post(std::string_view path, HandlerVariant handler);

  /**
   * @brief Start listening on a port.
   * @param port Port number to listen on.
   */
  Result<void> listen(int port);

private:
  Dispatcher dispatcher_;
  Router router_;
};

} // namespace draco
