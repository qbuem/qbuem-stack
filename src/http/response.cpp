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

Response &Response::set_cookie(std::string_view name, std::string_view value,
                                CookieOptions opts) {
  std::string cookie = std::string(name) + "=" + std::string(value);

  if (!opts.path.empty())
    cookie += "; Path=" + opts.path;
  if (!opts.domain.empty())
    cookie += "; Domain=" + opts.domain;
  if (opts.max_age >= 0)
    cookie += "; Max-Age=" + std::to_string(opts.max_age);
  if (!opts.same_site.empty())
    cookie += "; SameSite=" + opts.same_site;
  if (opts.secure)
    cookie += "; Secure";
  if (opts.http_only)
    cookie += "; HttpOnly";

  cookies_.push_back(std::move(cookie));
  return *this;
}

std::string Response::serialize_header() const {
  // Pre-size: rough estimate to avoid reallocations in the common case.
  // Status line (~20B) + ~4 headers * ~40B + Content-Length (~30B) + CRLF (2B)
  std::string hdr;
  hdr.reserve(256 + headers_.size() * 48);

  hdr += "HTTP/1.1 ";
  hdr += std::to_string(status_code_);
  hdr += ' ';
  hdr += status_to_string(status_code_);
  hdr += "\r\n";

  for (const auto &[key, value] : headers_) {
    hdr += key;
    hdr += ": ";
    hdr += value;
    hdr += "\r\n";
  }
  for (const auto &c : cookies_) {
    hdr += "Set-Cookie: ";
    hdr += c;
    hdr += "\r\n";
  }
  hdr += "Content-Length: ";
  hdr += std::to_string(sendfile_path_.empty() ? body_.size() : sendfile_size_);
  hdr += "\r\n\r\n";
  return hdr;
}

std::string Response::serialize() const {
  std::string hdr = serialize_header();
  hdr += body_;
  return hdr;
}

Response &Response::sendfile_path(std::string_view path, size_t size) {
  sendfile_path_ = std::string(path);
  sendfile_size_ = size;
  return *this;
}

std::string_view Response::status_to_string(int code) const {
  switch (code) {
  case 200: return "OK";
  case 201: return "Created";
  case 204: return "No Content";
  case 206: return "Partial Content";
  case 301: return "Moved Permanently";
  case 302: return "Found";
  case 304: return "Not Modified";
  case 400: return "Bad Request";
  case 401: return "Unauthorized";
  case 403: return "Forbidden";
  case 404: return "Not Found";
  case 405: return "Method Not Allowed";
  case 408: return "Request Timeout";
  case 429: return "Too Many Requests";
  case 500: return "Internal Server Error";
  case 502: return "Bad Gateway";
  case 503: return "Service Unavailable";
  default:  return "Unknown";
  }
}

} // namespace draco
