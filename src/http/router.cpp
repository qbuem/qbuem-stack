#include <qbuem/http/router.hpp>

#include <algorithm>
#include <ranges>

namespace qbuem {

// RadixTree Implementation
RadixTree::RadixTree() : root_(std::make_unique<Node>()) {}
RadixTree::~RadixTree() = default;

void RadixTree::insert(std::string_view path, HandlerVariant handler) {
  Node *curr = root_.get();
  size_t i = 0;
  while (i < path.length()) {
    if (path[i] == ':') {
      size_t j = i + 1;
      while (j < path.length() && path[j] != '/')
        j++;
      std::string param_name(path.substr(i + 1, j - i - 1));
      if (!curr->param_child) {
        curr->param_child = std::make_unique<Node>();
        curr->param_name  = std::move(param_name);
      }
      curr = curr->param_child.get();
      i = j;
    } else {
      Node *child = curr->find_child(path[i]);
      if (child == nullptr) {
        curr->children.emplace_back(path[i], std::make_unique<Node>());
        child = curr->children.back().second.get();
      }
      curr = child;
      i++;
    }
  }
  curr->handler = std::move(handler);
}

HandlerVariant
RadixTree::search(std::string_view path,
                  std::unordered_map<std::string, std::string> &params) const {
  return search_recursive(root_.get(), path, 0, params);
}

HandlerVariant RadixTree::search_recursive(
    const Node *node, std::string_view path, size_t pos,
    std::unordered_map<std::string, std::string> &params) const {
  if (pos == path.length()) {
    return node->handler;
  }

  // Try exact character child first
  const Node *child = node->find_child(path[pos]);
  if (child != nullptr) {
    auto res = search_recursive(child, path, pos + 1, params);
    if (!std::holds_alternative<std::monostate>(res))
      return res;
  }

  // Try param child
  if (node->param_child) {
    const Node *param_node = node->param_child.get();
    size_t next_slash = path.find('/', pos);
    if (next_slash == std::string_view::npos)
      next_slash = path.length();

    std::string val(path.substr(pos, next_slash - pos));
    params[node->param_name] = val;

    auto res = search_recursive(param_node, path, next_slash, params);
    if (!std::holds_alternative<std::monostate>(res))
      return res;

    params.erase(node->param_name);
  }

  return std::monostate{};
}

// Router Implementation
void Router::add_route(Method method, std::string_view path,
                       HandlerVariant handler) {
  routes_[static_cast<size_t>(method)].insert(path, std::move(handler));
}

void Router::use(Middleware mw) { middlewares_.emplace_back(std::move(mw)); }

void Router::use_async(AsyncMiddleware mw) {
  middlewares_.emplace_back(std::move(mw));
  has_async_mw_ = true;
}

void Router::add_prefix_route(Method method, std::string_view prefix,
                              HandlerVariant handler) {
  prefix_routes_.push_back({method, std::string(prefix), std::move(handler)});
}

HandlerVariant
Router::match(Method method, std::string_view path,
              std::unordered_map<std::string, std::string> &params) const {
  // 1. Exact / param match via RadixTree — direct array index, no hash
  auto h = routes_[static_cast<size_t>(method)].search(path, params);
  if (!std::holds_alternative<std::monostate>(h))
    return h;

  // 2. Prefix routes (e.g., for static file serving)
  for (const auto &pr : prefix_routes_) {
    if (pr.method == method && path.starts_with(pr.prefix)) {
      params["**"] = std::string(path.substr(pr.prefix.size()));
      return pr.handler;
    }
  }

  return std::monostate{};
}

bool Router::path_exists(std::string_view path) const {
  std::unordered_map<std::string, std::string> dummy;
  for (size_t i = 0; i < kMethodCount; ++i) {
    if (!std::holds_alternative<std::monostate>(routes_[i].search(path, dummy)))
      return true;
    dummy.clear();
  }
  // Also check prefix routes
  return std::ranges::any_of(prefix_routes_,
      [&](const auto& pr) { return path.starts_with(pr.prefix); });
}

} // namespace qbuem
