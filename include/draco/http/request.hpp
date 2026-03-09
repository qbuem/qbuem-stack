#pragma once

#include <draco/common.hpp>

#include <beast_json/beast_json.hpp>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace draco {

enum class Method { Get, Post, Put, Delete, Patch, Options, Head, Unknown };

inline Method string_to_method(std::string_view m) {
  if (m == "GET")
    return Method::Get;
  if (m == "POST")
    return Method::Post;
  if (m == "PUT")
    return Method::Put;
  if (m == "DELETE")
    return Method::Delete;
  if (m == "PATCH")
    return Method::Patch;
  if (m == "OPTIONS")
    return Method::Options;
  if (m == "HEAD")
    return Method::Head;
  return Method::Unknown;
}

/**
 * @brief Zero-copy HTTP Request representation.
 */
class Request {
public:
  Request() = default;

  Method method() const { return method_; }
  std::string_view path() const { return path_; }
  std::string_view body() const { return body_; }

  std::string_view header(std::string_view key) const {
    auto it = headers_.find(std::string(key));
    return (it != headers_.end()) ? it->second : std::string_view{};
  }

  std::string_view param(std::string_view key) const {
    auto it = params_.find(std::string(key));
    return (it != params_.end()) ? it->second : std::string_view{};
  }

  // Setters for the parser/router
  void set_method(Method m) { method_ = m; }
  void set_path(std::string_view p) { path_ = p; }
  void set_body(std::string_view b) { body_ = b; }
  void add_header(std::string_view key, std::string_view value) {
    headers_[std::string(key)] = std::string(value);
  }
  void set_param(std::string_view key, std::string_view value) {
    params_[std::string(key)] = std::string(value);
  }

  /**
   * @brief Access the request body as JSON.
   */
  beast::Value json();

private:
  Method method_ = Method::Unknown;
  std::string path_; // Changed from std::string_view
  std::string body_; // Changed from std::string_view
  std::unordered_map<std::string, std::string>
      headers_; // Changed from std::vector<std::pair<std::string_view,
                // std::string_view>>
  std::unordered_map<std::string, std::string>
      params_; // Changed from std::unordered_map<std::string_view,
               // std::string_view>

  // Cached JSON view
  beast::Document json_doc_;
  std::optional<beast::Value> cached_json_;
};

} // namespace draco
