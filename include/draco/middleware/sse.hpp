#pragma once

/**
 * @file draco/middleware/sse.hpp
 * @brief Server-Sent Events (SSE) helper — text/event-stream streaming.
 *
 * Server-Sent Events allow a server to push data to a browser client over a
 * persistent HTTP/1.1 connection using chunked transfer encoding.
 *
 * Usage (sync handler example):
 *   app.get("/events", Handler([](const Request &, Response &res) {
 *     SseStream sse(res);
 *     sse.send("hello", "message");      // event: message\ndata: hello\n\n
 *     sse.send("42",    "counter", "1"); // event: counter\ndata: 42\nid: 1\n\n
 *     sse.heartbeat();                   // ": ping\n\n"
 *     sse.close();
 *   }));
 *
 * Note: For long-lived push streams, use an AsyncHandler with co_await sleep()
 * between events.  SseStream::send() calls Response::chunk() which buffers the
 * event; the buffer is sent when the handler returns (or when close() is called
 * which calls end_chunks()).
 */

#include <draco/http/response.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace draco {

/**
 * @brief SSE stream builder.
 *
 * Wraps a Response and provides send(), heartbeat(), and close() helpers.
 * Automatically sets Content-Type: text/event-stream and Cache-Control:
 * no-cache on the first send.
 */
class SseStream {
public:
  explicit SseStream(Response &res) : res_(res) {
    res_.status(200)
        .header("Content-Type", "text/event-stream; charset=utf-8")
        .header("Cache-Control", "no-cache")
        .header("X-Accel-Buffering", "no"); // disable nginx buffering
  }

  /**
   * @brief Send an SSE event.
   *
   * @param data   Payload (may contain newlines; each line is prefixed with "data: ").
   * @param event  Optional event type (sets the "event:" field).
   * @param id     Optional event ID (sets the "id:" field for last-event-id).
   * @param retry  Optional reconnect interval in milliseconds (sets "retry:").
   */
  SseStream &send(std::string_view data,
                  std::string_view event = {},
                  std::string_view id    = {},
                  int              retry = -1) {
    std::string frame;
    frame.reserve(16 + event.size() + id.size() + data.size());

    if (!event.empty()) {
      frame += "event: ";
      frame += event;
      frame += '\n';
    }
    if (!id.empty()) {
      frame += "id: ";
      frame += id;
      frame += '\n';
    }
    if (retry >= 0) {
      frame += "retry: ";
      frame += std::to_string(retry);
      frame += '\n';
    }

    // Split multi-line data across multiple "data:" fields.
    size_t start = 0;
    while (start < data.size()) {
      auto nl = data.find('\n', start);
      frame += "data: ";
      if (nl == std::string_view::npos) {
        frame += data.substr(start);
        start = data.size();
      } else {
        frame += data.substr(start, nl - start);
        start = nl + 1;
      }
      frame += '\n';
    }
    frame += '\n'; // blank line terminates the event

    res_.chunk(frame);
    return *this;
  }

  /**
   * @brief Send a heartbeat comment to keep the connection alive.
   *
   * Browsers reconnect after ~30 s of silence; send a heartbeat every ~15 s.
   */
  SseStream &heartbeat() {
    res_.chunk(": ping\n\n");
    return *this;
  }

  /**
   * @brief Finalize the stream.
   *
   * Appends the terminal chunked-encoding terminator.  After close(), no more
   * events can be sent.  The response is sent when the handler returns.
   */
  void close() {
    if (!closed_) {
      res_.end_chunks();
      closed_ = true;
    }
  }

  ~SseStream() { close(); }

private:
  Response &res_;
  bool      closed_ = false;
};

} // namespace draco
