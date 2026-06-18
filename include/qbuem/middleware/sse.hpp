#pragma once

/**
 * @file qbuem/middleware/sse.hpp
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
 * Two modes:
 *   - send() (sync): buffers via Response::chunk(); the buffer reaches the socket
 *     when the handler RETURNS. Fine for a bounded burst, NOT for live streaming.
 *   - send_async() (co_await, since v1.4.1): flushes each event to the socket
 *     mid-handler via Response::flush(), so a long-lived
 *       for(;;){ co_await sse.send_async(json,"ev"); co_await qbuem::sleep(1000); }
 *     loop streams live. Use this for SSE push. Requires an AsyncHandler (the
 *     connection injects the socket fd for async routes). End with close_async().
 */

#include <qbuem/http/response.hpp>
#include <qbuem/core/task.hpp>   // Task<void> for send_async/close_async

#include <cstdint>
#include <string>
#include <string_view>

namespace qbuem {

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
    res_.chunk(build_frame(data, event, id, retry));   // buffered — flushes at return
    return *this;
  }

  /**
   * @brief Send an SSE event AND flush it to the socket immediately (streaming).
   *
   * Unlike send() (which buffers until the handler returns), send_async() writes
   * the event to the socket mid-handler via Response::flush(), so a long-lived
   * `for(;;){ send_async(...); co_await sleep(...); }` loop streams live. Requires
   * an async handler whose connection injected the socket fd (App::listen does so
   * for AsyncHandler routes). On the connection's reactor thread.
   */
  Task<void> send_async(std::string_view data,
                        std::string_view event = {},
                        std::string_view id    = {},
                        int              retry = -1) {
    res_.chunk(build_frame(data, event, id, retry));
    co_await res_.flush();
  }

  /** @brief Flush the terminal chunk to close a streamed SSE response. */
  Task<void> close_async() {
    if (!closed_) {
      co_await res_.flush_end();
      closed_ = true;
    }
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
  // Build the raw SSE wire frame (event:/id:/retry: lines + data: line(s) + blank).
  static std::string build_frame(std::string_view data, std::string_view event,
                                 std::string_view id, int retry) {
    std::string frame;
    frame.reserve(16 + event.size() + id.size() + data.size());
    if (!event.empty()) { frame += "event: "; frame += event; frame += '\n'; }
    if (!id.empty())    { frame += "id: ";    frame += id;    frame += '\n'; }
    if (retry >= 0)     { frame += "retry: "; frame += std::to_string(retry); frame += '\n'; }
    size_t start = 0;
    while (start < data.size()) {
      auto nl = data.find('\n', start);
      frame += "data: ";
      if (nl == std::string_view::npos) { frame += data.substr(start); start = data.size(); }
      else { frame += data.substr(start, nl - start); start = nl + 1; }
      frame += '\n';
    }
    frame += '\n'; // blank line terminates the event
    return frame;
  }

  Response &res_;
  bool      closed_ = false;
};

} // namespace qbuem
