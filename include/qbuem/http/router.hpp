#ifndef QBUEM_HTTP_ROUTER_HPP
#define QBUEM_HTTP_ROUTER_HPP

#include <qbuem/core/task.hpp>
#include <qbuem/http/request.hpp>
#include <qbuem/http/response.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant> // Added for std::variant and std::monostate
#include <vector>

namespace qbuem {

using Handler = std::function<void(const Request &, Response &)>;
using AsyncHandler = std::function<Task<void>(const Request &, Response &)>;
using HandlerVariant = std::variant<std::monostate, Handler, AsyncHandler>;

/** @brief 동기 미들웨어 — false 반환 시 체인 중단. */
using Middleware = std::function<bool(const Request &, Response &)>;

/**
 * @brief next() 기반 비동기 미들웨어.
 *
 * `next()`를 co_await 하면 체인의 나머지(다음 미들웨어 또는 라우트 핸들러)가
 * 실행됩니다.  next()를 호출하지 않으면 체인이 중단됩니다.
 *
 * 예시:
 * @code
 * app.use_async([](const qbuem::Request& req, qbuem::Response& res,
 *                  qbuem::NextFn next) -> qbuem::Task<bool> {
 *   // 전처리
 *   co_await next();
 *   // 후처리 (응답이 채워진 후)
 *   co_return true;
 * });
 * @endcode
 */
using NextFn = std::function<Task<void>()>;
using AsyncMiddleware = std::function<Task<bool>(const Request &, Response &, NextFn)>;

/** @brief 동기 / 비동기 미들웨어를 하나의 타입으로 저장하는 Variant. */
using AnyMiddleware = std::variant<Middleware, AsyncMiddleware>;

/**
 * @brief Error handler called when a route handler throws an exception.
 *
 * Parameters: exception_ptr, request, response.
 * If not set, the default behaviour is 500 Internal Server Error.
 *
 * Example:
 *   app.on_error([](std::exception_ptr ep, const Request& req, Response& res) {
 *     try { std::rethrow_exception(ep); }
 *     catch (const std::exception& e) {
 *       res.status(500).body(e.what());
 *     }
 *   });
 */
using ErrorHandler =
    std::function<void(std::exception_ptr, const Request &, Response &)>;

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
    // Compact sorted children: key=first-char, value=child node.
    // Children are kept sorted by key so binary search can be used when
    // branching factor exceeds kBinarySearchThreshold (default: 4).
    std::vector<std::pair<char, std::unique_ptr<Node>>> children;
    std::unique_ptr<Node> param_child; // ':' wildcard child
    std::string           param_name;  // param name if param_child is set
    HandlerVariant        handler;

    static constexpr size_t kBinarySearchThreshold = 4;

    const Node *find_child(char c) const noexcept {
      if (children.size() > kBinarySearchThreshold) {
        // Binary search on sorted children.
        auto it = std::lower_bound(children.begin(), children.end(), c,
            [](const auto &p, char ch) { return p.first < ch; });
        if (it != children.end() && it->first == c) return it->second.get();
        return nullptr;
      }
      for (auto &[k, v] : children)
        if (k == c) return v.get();
      return nullptr;
    }
    Node *find_child(char c) noexcept {
      if (children.size() > kBinarySearchThreshold) {
        auto it = std::lower_bound(children.begin(), children.end(), c,
            [](const auto &p, char ch) { return p.first < ch; });
        if (it != children.end() && it->first == c) return it->second.get();
        return nullptr;
      }
      for (auto &[k, v] : children)
        if (k == c) return v.get();
      return nullptr;
    }

    // Insert a child in sorted order (maintains sorted invariant for binary search).
    Node *add_child(char c) {
      auto it = std::lower_bound(children.begin(), children.end(), c,
          [](const auto &p, char ch) { return p.first < ch; });
      auto ins = children.emplace(it, c, std::make_unique<Node>());
      return ins->second.get();
    }
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

  /** @brief 동기 미들웨어를 체인에 추가합니다. */
  void use(Middleware mw);

  /** @brief next() 기반 비동기 미들웨어를 체인에 추가합니다. */
  void use_async(AsyncMiddleware mw);

  HandlerVariant
  match(Method method, std::string_view path,
        std::unordered_map<std::string, std::string> &params) const;

  /**
   * Register a prefix route that matches all paths beginning with @p prefix.
   *
   * The matched suffix (everything after the prefix) is available via
   * req.param("**").
   *
   * Used internally by App::serve_static() to serve an entire directory tree
   * without requiring wildcard support in the RadixTree.
   */
  void add_prefix_route(Method method, std::string_view prefix,
                        HandlerVariant handler);

  /**
   * Return true if @p path is registered for ANY method.
   * Used to distinguish 404 (path unknown) from 405 (method not allowed).
   */
  bool path_exists(std::string_view path) const;

  const std::vector<AnyMiddleware> &middlewares() const { return middlewares_; }

  /** @brief 비동기 미들웨어가 하나라도 등록되어 있으면 true. */
  bool has_async_middlewares() const { return has_async_mw_; }

private:
  struct PrefixRoute {
    Method        method;
    std::string   prefix;
    HandlerVariant handler;
  };

  // Direct-indexed array: Method enum values are 0..7 (Get..Unknown).
  static constexpr size_t kMethodCount = 8;
  std::array<RadixTree, kMethodCount> routes_;
  std::vector<AnyMiddleware>            middlewares_;
  std::vector<PrefixRoute>             prefix_routes_;
  bool                                  has_async_mw_ = false;
};

} // namespace qbuem

#endif // QBUEM_HTTP_ROUTER_HPP
