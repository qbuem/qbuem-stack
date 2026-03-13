#pragma once

/**
 * @file qbuem/middleware/static_files.hpp
 * @brief Static file serving helpers: MIME detection, ETag generation.
 *
 * Used internally by App::serve_static().  Can also be used standalone to
 * compose custom file-serving handlers.
 */

#include <qbuem/http/request.hpp>
#include <qbuem/http/response.hpp>

#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unordered_map>
#ifdef __linux__
#  include <fcntl.h>
#  include <sys/sendfile.h>
#endif

namespace qbuem::middleware {

// ─── MIME type table ──────────────────────────────────────────────────────────

/**
 * Return the MIME type for a file extension (including the leading dot).
 * Returns "application/octet-stream" for unknown extensions.
 */
inline std::string_view mime_type(std::string_view ext) {
  // Static table: extension → MIME type.
  static const std::unordered_map<std::string_view, std::string_view> kTable{
      // Text
      {".html",  "text/html; charset=utf-8"},
      {".htm",   "text/html; charset=utf-8"},
      {".css",   "text/css; charset=utf-8"},
      {".js",    "text/javascript; charset=utf-8"},
      {".mjs",   "text/javascript; charset=utf-8"},
      {".txt",   "text/plain; charset=utf-8"},
      {".md",    "text/markdown; charset=utf-8"},
      {".csv",   "text/csv; charset=utf-8"},
      // Data
      {".json",  "application/json"},
      {".xml",   "application/xml"},
      {".pdf",   "application/pdf"},
      {".wasm",  "application/wasm"},
      {".zip",   "application/zip"},
      {".gz",    "application/gzip"},
      // Images
      {".png",   "image/png"},
      {".jpg",   "image/jpeg"},
      {".jpeg",  "image/jpeg"},
      {".gif",   "image/gif"},
      {".svg",   "image/svg+xml"},
      {".ico",   "image/x-icon"},
      {".webp",  "image/webp"},
      {".avif",  "image/avif"},
      // Fonts
      {".woff",  "font/woff"},
      {".woff2", "font/woff2"},
      {".ttf",   "font/ttf"},
      {".otf",   "font/otf"},
      // Audio / Video
      {".mp3",   "audio/mpeg"},
      {".ogg",   "audio/ogg"},
      {".mp4",   "video/mp4"},
      {".webm",  "video/webm"},
  };
  auto it = kTable.find(ext);
  return (it != kTable.end()) ? it->second : "application/octet-stream";
}

/**
 * Extract the file extension from a path (e.g., "/foo/bar.js" → ".js").
 * Returns empty string_view if no extension is present.
 */
inline std::string_view file_extension(std::string_view path) {
  size_t dot = path.rfind('.');
  if (dot == std::string_view::npos) return {};
  size_t slash = path.rfind('/');
  if (slash != std::string_view::npos && dot < slash) return {};
  return path.substr(dot);
}

// ─── File info ────────────────────────────────────────────────────────────────

/**
 * Serve a file at @p fs_path into @p res.
 *
 * On success: sets status 200, Content-Type, ETag, Last-Modified, and body.
 * On not found:  sets status 404.
 * On read error: sets status 500.
 *
 * The ETag is a weak tag of the form W/"<size>-<mtime>".
 */
inline void serve_file(std::string_view fs_path, Response &res) {
  std::string path_str(fs_path);
  struct ::stat st{};
  if (::stat(path_str.c_str(), &st) != 0) {
    res.status(404).body("Not Found");
    return;
  }

  // Only serve regular files.
  if (!S_ISREG(st.st_mode)) {
    res.status(404).body("Not Found");
    return;
  }

  // Build weak ETag: W/"<size>-<mtime>"
  std::string etag_val = "W/\"" + std::to_string(st.st_size) + "-" +
                         std::to_string(st.st_mtime) + "\"";

  // MIME type from extension.
  auto ext  = file_extension(fs_path);
  auto mime = mime_type(ext);

  res.status(200)
     .header("Content-Type", mime)
     .header("ETag", etag_val)
     .last_modified(static_cast<std::time_t>(st.st_mtime));

#ifdef __linux__
  // Zero-copy path: store file path; qbuem.cpp send loop will use sendfile(2).
  res.sendfile_path(path_str, static_cast<size_t>(st.st_size));
#else
  // Portable fallback: read into body.
  std::ifstream f(path_str, std::ios::binary);
  if (!f) {
    res.status(500).body("Internal Server Error");
    return;
  }
  std::string content((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
  res.body(content);
#endif
}

} // namespace qbuem::middleware
