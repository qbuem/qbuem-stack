#include <draco/http/response.hpp>
#include <sstream>

namespace draco {

Response &Response::status(int code) {
  status_code_ = code;
  return *this;
}

Response &Response::header(std::string_view key, std::string_view value) {
  headers_[std::string(key)] = std::string(value);
  return *this;
}

Response &Response::body(std::string_view b) {
  body_ = std::string(b);
  return *this;
}

std::string Response::serialize() const {
  std::stringstream ss;
  ss << "HTTP/1.1 " << status_code_ << " " << status_to_string(status_code_)
     << "\r\n";
  for (const auto &[key, value] : headers_) {
    ss << key << ": " << value << "\r\n";
  }
  ss << "Content-Length: " << body_.size() << "\r\n";
  ss << "\r\n";
  ss << body_;
  return ss.str();
}

std::string_view Response::status_to_string(int code) const {
  switch (code) {
  case 200:
    return "OK";
  case 201:
    return "Created";
  case 204:
    return "No Content";
  case 206:
    return "Partial Content";
  case 301:
    return "Moved Permanently";
  case 302:
    return "Found";
  case 304:
    return "Not Modified";
  case 400:
    return "Bad Request";
  case 401:
    return "Unauthorized";
  case 403:
    return "Forbidden";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 408:
    return "Request Timeout";
  case 429:
    return "Too Many Requests";
  case 500:
    return "Internal Server Error";
  case 502:
    return "Bad Gateway";
  case 503:
    return "Service Unavailable";
  default:
    return "Unknown";
  }
}

} // namespace draco
