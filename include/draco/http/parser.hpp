#pragma once

#include <draco/http/request.hpp>
#include <optional>
#include <string_view>

namespace draco {

/**
 * @brief Simple state-machine based HTTP/1.1 parser.
 */
class HttpParser {
public:
  enum class State {
    Method,
    Path,
    Version,
    HeaderKey,
    HeaderValue,
    Body,
    Complete,
    Error
  };

  HttpParser() = default;

  std::optional<size_t> parse(std::string_view data, Request &req);

  bool is_complete() const { return state_ == State::Complete; }
  State state() const { return state_; }

private:
  State state_ = State::Method;
  std::string_view current_header_key_;
  size_t body_length_ = 0;
};

} // namespace draco
