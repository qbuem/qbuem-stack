#include <draco/http/parser.hpp>

#include <beast_json/beast_json.hpp>
#include <iostream>
#include <string>

namespace draco {

std::optional<size_t> HttpParser::parse(std::string_view data, Request &req) {
  size_t pos = 0;
  size_t start = 0;

  while (pos < data.size() && state_ != State::Complete &&
         state_ != State::Error) {
    char c = data[pos];

    switch (state_) {
    case State::Method:
      if (c == ' ') {
        req.set_method(string_to_method(data.substr(start, pos - start)));
        state_ = State::Path;
        start = pos + 1;
      }
      break;

    case State::Path:
      if (c == ' ') {
        req.set_path(data.substr(start, pos - start));
        state_ = State::Version;
        start = pos + 1;
      }
      break;

    case State::Version:
      if (c == '\r') {
        // Skip version for now (HTTP/1.1 assumed)
      } else if (c == '\n') {
        state_ = State::HeaderKey;
        start = pos + 1;
      }
      break;

    case State::HeaderKey:
      if (c == ':') {
        current_header_key_ = data.substr(start, pos - start);
        state_ = State::HeaderValue;
        start = pos + 1;
        // Skip optional space after colon
        if (pos + 1 < data.size() && data[pos + 1] == ' ') {
          pos++;
          start++;
        }
      } else if (c == '\r') {
        // Potential end of headers
      } else if (c == '\n') {
        // End of headers
        state_ = State::Body;
        start = pos + 1;
        // Check Content-Length for body parsing
        std::string_view cl = req.header("Content-Length");
        if (!cl.empty()) {
          body_length_ = std::stoul(std::string(cl));
        } else {
          state_ = State::Complete;
        }
      }
      break;

    case State::HeaderValue:
      if (c == '\r') {
        // End of value
      } else if (c == '\n') {
        req.add_header(current_header_key_,
                       data.substr(start, pos - start - 1)); // -1 for \r
        state_ = State::HeaderKey;
        start = pos + 1;
      }
      break;

    case State::Body:
      if (data.size() - start >= body_length_) {
        req.set_body(data.substr(start, body_length_));
        pos = start + body_length_ - 1; // -1 because pos++ at end
        state_ = State::Complete;
      } else {
        // Need more data
        return pos;
      }
      break;

    default:
      break;
    }
    pos++;
  }

  if (state_ == State::Complete)
    return pos;
  if (state_ == State::Error)
    return std::nullopt;
  return pos; // Incomplete
}

} // namespace draco
