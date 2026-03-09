#ifndef DRACO_HTTP_ROUTER_HPP
#define DRACO_HTTP_ROUTER_HPP

#include <draco/core/task.hpp>
#include <draco/http/request.hpp>
#include <draco/http/response.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant> // Added for std::variant and std::monostate
#include <vector>

namespace draco {

using Handler = std::function<void(const Request &, Response &)>;
using AsyncHandler = std::function<Task<void>(const Request &, Response &)>;
using HandlerVariant = std::variant<std::monostate, Handler, AsyncHandler>;
using Middleware = std::function<bool(const Request &, Response &)>;

/**
 * @brief Radix Tree for high-performance route matching.
 */
class RadixTree {
public:
  RadixTree();
  ~RadixTree();
  void insert(std::string_view path, HandlerVariant handler);
  HandlerVariant
  search(std::string_view path,
         std::unordered_map<std::string, std::string> &params) const;

private:
  struct Node {
    std::unordered_map<char, std::unique_ptr<Node>> children;
    HandlerVariant handler;
    bool is_param = false;
    std::string param_name;
  };

  HandlerVariant
  search_recursive(const Node *node, std::string_view path, size_t pos,
                   std::unordered_map<std::string, std::string> &params) const;

  std::unique_ptr<Node> root_;
};

/**
 * @brief Router handles HTTP route matching and middleware.
 */
class Router {
public:
  void add_route(Method method, std::string_view path, HandlerVariant handler);
  void use(Middleware mw);

  HandlerVariant
  match(Method method, std::string_view path,
        std::unordered_map<std::string, std::string> &params) const;

  const std::vector<Middleware> &middlewares() const { return middlewares_; }

private:
  std::unordered_map<Method, RadixTree> routes_;
  std::vector<Middleware> middlewares_;
};

} // namespace draco

#endif // DRACO_HTTP_ROUTER_HPP
