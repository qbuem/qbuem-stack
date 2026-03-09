#include <draco/http/request.hpp>

namespace draco {

std::string_view Request::header(std::string_view key) const {
  auto it = headers_.find(std::string(key));
  return (it != headers_.end()) ? it->second : "";
}

void Request::add_header(std::string_view key, std::string_view value) {
  headers_[std::string(key)] = std::string(value);
}

void Request::set_param(std::string_view key, std::string_view value) {
  params_[std::string(key)] = std::string(value);
}

std::string_view Request::param(std::string_view key) const {
  auto it = params_.find(std::string(key));
  return (it != params_.end()) ? it->second : "";
}

beast::Value Request::json() {
  if (!cached_json_) {
    cached_json_ = beast::parse(json_doc_, body_);
  }
  return *cached_json_;
}

} // namespace draco
