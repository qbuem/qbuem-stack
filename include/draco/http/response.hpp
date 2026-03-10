#pragma once

#include <draco/common.hpp>

#include <string>
#include <string_view>
#include <unordered_map>

namespace draco {

/**
 * @brief HTTP Response builder.
 *
 * Use body() with a pre-serialized string for structured data (JSON, etc.).
 * There is intentionally no built-in JSON method — serialize with whichever
 * library you choose and pass the result to body(). See examples/coro_json.cpp.
 */
class Response {
public:
  Response() = default;

  Response &status(int code);
  Response &header(std::string_view key, std::string_view value);
  Response &body(std::string_view b);

  std::string serialize() const;

private:
  std::string_view status_to_string(int code) const;

  int status_code_ = 200;
  std::unordered_map<std::string, std::string> headers_;
  std::string body_;
};

} // namespace draco
