#include <draco/http/request.hpp>

namespace draco {

beast::Value Request::json() const {
  if (!cached_json_) {
    if (body_.empty()) {
      cached_json_ = beast::Value{};
    } else {
      try {
        cached_json_ = beast::parse(json_doc_, body_);
      } catch (const std::exception &) {
        cached_json_ = beast::Value{};
      }
    }
  }
  return *cached_json_;
}

} // namespace draco
