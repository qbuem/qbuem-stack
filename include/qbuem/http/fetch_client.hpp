#pragma once

/**
 * @file qbuem/http/fetch_client.hpp
 * @brief Persistent HTTP client with connection pooling and Keep-Alive support.
 * @defgroup qbuem_fetch_client HTTP FetchClient
 * @ingroup qbuem_http
 *
 * ## Overview
 * `fetch()` in `fetch.hpp` uses `Connection: close` — it creates a new TCP
 * connection for every request. `FetchClient` maintains a per-host connection
 * pool so that consecutive requests to the same host reuse existing TCP
 * connections, eliminating the TCP handshake and DNS overhead on the hot path.
 *
 * ## Connection lifecycle
 * 1. `FetchClient::request(url)` returns a `ClientRequest` builder.
 * 2. `co_await req.send(st)` acquires a pooled connection (or creates a new one).
 * 3. The request is sent with `Connection: keep-alive`.
 * 4. On response:
 *    - If the server replies `Connection: keep-alive` (or omits `Connection: close`
 *      for HTTP/1.1), the socket is returned to the pool.
 *    - Otherwise the socket is closed.
 * 5. Idle connections beyond `max_idle_per_host` are evicted immediately.
 *
 * ## Thread safety
 * `FetchClient` is **NOT** thread-safe. All calls must occur on the same
 * reactor thread. Use one `FetchClient` per reactor thread.
 *
 * ## Usage
 * @code
 * FetchClient client;
 * client.set_max_idle_per_host(4);
 * client.set_timeout(std::chrono::seconds{10});
 *
 * // First request: DNS + TCP handshake
 * auto r1 = co_await client.request("http://httpbin.org/get").send(st);
 *
 * // Second request to same host: reuses existing TCP connection
 * auto r2 = co_await client.request("http://httpbin.org/post")
 *               .post()
 *               .body(R"({"key":"val"})")
 *               .send(st);
 *
 * // Monadic chaining works identically to fetch()
 * auto body = r2.transform([](const FetchResponse& r){ return std::string(r.body()); })
 *               .value_or("(error)");
 * @endcode
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/http/fetch.hpp>
#include <qbuem/net/dns.hpp>
#include <qbuem/net/tcp_stream.hpp>

#include <charconv>
#include <chrono>
#include <deque>
#include <stop_token>
#include <string>
#include <unordered_map>

namespace qbuem {

// Forward declaration
class FetchClient;

// ─── ClientRequest ───────────────────────────────────────────────────────────

/**
 * @brief HTTP request builder for use with `FetchClient`.
 *
 * Behaves like `FetchRequest` but acquires connections from the owning
 * `FetchClient`'s pool and returns them when done.
 */
class ClientRequest {
public:
  ClientRequest(FetchClient *client, std::string url)
      : client_(client), url_(std::move(url)) {}

  /** @brief Set the HTTP method (default: GET). */
  ClientRequest &method(Method m)                  { method_ = m;              return *this; }
  /** @brief Add a request header. */
  ClientRequest &header(std::string_view k, std::string_view v) {
    headers_[std::string(k)] = std::string(v);
    return *this;
  }
  /** @brief Set the request body. */
  ClientRequest &body(std::string_view b)          { body_ = b;                return *this; }
  /** @brief Set method to GET. */
  ClientRequest &get()   { method_ = Method::Get;    return *this; }
  /** @brief Set method to POST. */
  ClientRequest &post()  { method_ = Method::Post;   return *this; }
  /** @brief Set method to PUT. */
  ClientRequest &put()   { method_ = Method::Put;    return *this; }
  /** @brief Set method to DELETE. */
  ClientRequest &del()   { method_ = Method::Delete; return *this; }
  /** @brief Set method to PATCH. */
  ClientRequest &patch() { method_ = Method::Patch;  return *this; }

  /**
   * @brief Execute the request using the client's connection pool.
   *
   * @param st  Cancellation token.
   */
  Task<Result<FetchResponse>> send(std::stop_token st = {});

private:
  FetchClient  *client_;
  std::string   url_;
  Method        method_ = Method::Get;
  StringMap     headers_;
  std::string   body_;
};

// ─── FetchClient ─────────────────────────────────────────────────────────────

/**
 * @brief HTTP client with per-host TCP connection pooling and Keep-Alive.
 *
 * Maintains a free-list of idle `TcpStream` sockets keyed by "host:port".
 * Connections are kept alive and reused across requests to the same endpoint,
 * reducing latency on repeat requests.
 *
 * ### Not thread-safe
 * Use one `FetchClient` instance per reactor thread. Do not share across threads.
 */
class FetchClient {
public:
  /** @brief Construct a client with default settings. */
  FetchClient() = default;

  /** @brief Non-copyable (owns TCP connections). */
  FetchClient(const FetchClient &) = delete;
  FetchClient &operator=(const FetchClient &) = delete;

  /** @brief Movable. */
  FetchClient(FetchClient &&)            = default;
  FetchClient &operator=(FetchClient &&) = default;

  // ── Configuration ────────────────────────────────────────────────────────

  /**
   * @brief Set the maximum number of idle connections kept per host.
   * @param n  Maximum idle connections per "host:port" key (default: 4).
   */
  void set_max_idle_per_host(size_t n) { max_idle_per_host_ = n; }

  /**
   * @brief Set a default request timeout applied to all requests.
   * @param d  Timeout duration (default: 0 = no timeout).
   */
  void set_timeout(std::chrono::milliseconds d) { default_timeout_ms_ = d.count(); }

  /** @brief Overload accepting any duration type. */
  template <typename Rep, typename Period>
  void set_timeout(std::chrono::duration<Rep, Period> d) {
    default_timeout_ms_ =
        std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
  }

  /**
   * @brief Set the maximum number of 3xx redirects to follow (default: 5).
   * @param n  Redirect limit.
   */
  void set_max_redirects(int n) { default_max_redirects_ = n; }

  // ── Request factory ──────────────────────────────────────────────────────

  /**
   * @brief Create a request builder for the given URL.
   *
   * @param url  Target URL.
   * @returns    A `ClientRequest` builder bound to this client.
   */
  [[nodiscard]] ClientRequest request(std::string_view url) {
    return ClientRequest(this, std::string(url));
  }

  // ── Pool management (internal — used by ClientRequest::send) ─────────────

  /**
   * @brief Try to acquire an idle pooled connection for the given key.
   *
   * @param key  "host:port" string.
   * @returns    A moved TcpStream, or an empty optional if the pool is empty.
   */
  std::optional<TcpStream> acquire(const std::string &key) {
    auto it = pool_.find(key);
    if (it == pool_.end() || it->second.empty()) return std::nullopt;
    auto stream = std::move(it->second.back());
    it->second.pop_back();
    return stream;
  }

  /**
   * @brief Build a "host:port" pool key without heap allocation for the port.
   *
   * Uses std::to_chars() into a stack buffer to avoid the std::to_string()
   * temporary allocation on every request.
   */
  static std::string make_pool_key(std::string_view host, uint16_t port) {
    char port_buf[6]; // max "65535"
    auto [ptr, ec] = std::to_chars(port_buf, port_buf + sizeof(port_buf), port);
    std::string key;
    key.reserve(host.size() + 1 + static_cast<size_t>(ptr - port_buf));
    key.append(host);
    key += ':';
    key.append(port_buf, ptr);
    return key;
  }

  /**
   * @brief Return a connection to the pool after a keep-alive response.
   *
   * Evicts the oldest connection if the pool is at capacity.
   *
   * @param key     "host:port" string.
   * @param stream  Socket to return to the pool.
   */
  void release(const std::string &key, TcpStream stream) {
    auto &bucket = pool_[key];
    if (bucket.size() >= max_idle_per_host_) {
      bucket.pop_front(); // O(1) eviction of oldest via std::deque
    }
    bucket.push_back(std::move(stream));
  }

  /** @brief Return the default timeout in milliseconds (0 = none). */
  long long default_timeout_ms() const noexcept { return default_timeout_ms_; }

  /** @brief Return the default maximum redirect count. */
  int default_max_redirects() const noexcept { return default_max_redirects_; }

  /**
   * @brief Close and discard all pooled connections.
   *
   * Useful before destroying the client or during shutdown.
   */
  void clear_pool() { pool_.clear(); }

  /** @brief Return the total number of idle connections across all hosts. */
  size_t idle_count() const noexcept {
    size_t n = 0;
    for (auto &[k, v] : pool_) n += v.size();
    return n;
  }

private:
  // Pool: "host:port" → free-list of idle TcpStream sockets.
  // std::deque gives O(1) pop_front() for FIFO eviction of oldest connections.
  std::unordered_map<std::string, std::deque<TcpStream>> pool_;

  size_t    max_idle_per_host_     = 4;
  long long default_timeout_ms_    = 0;
  int       default_max_redirects_ = 5;
};

// ─── ClientRequest::send implementation ──────────────────────────────────────

inline Task<Result<FetchResponse>> ClientRequest::send(std::stop_token st) {
  // Parse URL
  auto parsed = ParsedUrl::parse(url_);
  if (!parsed) co_return unexpected(parsed.error());

  if (parsed->scheme == "https")
    co_return unexpected(
        std::make_error_code(std::errc::protocol_not_supported));

  // Pool key — uses to_chars to avoid to_string() heap allocation.
  std::string pool_key = FetchClient::make_pool_key(parsed->host, parsed->port);

  // Build combined stop_token (user + client timeout)
  std::stop_source timeout_ss;
  int              timer_id = -1;
  Reactor*         reactor  = Reactor::current();

  long long tms = client_->default_timeout_ms();
  if (tms > 0 && reactor) {
    auto r = reactor->register_timer(
        static_cast<int>(tms),
        [&timeout_ss](int) { timeout_ss.request_stop(); });
    if (r) timer_id = *r;
  }

  detail::CombinedStop combined{st, timeout_ss.get_token()};
  auto effective_st = combined.token();

  auto cancel_timer = [&] {
    if (timer_id >= 0 && reactor) reactor->unregister_timer(timer_id);
  };

  int redirects = 0;
  std::string current_url = url_;

  while (true) {
    if (effective_st.stop_requested()) {
      cancel_timer();
      co_return unexpected(
          std::make_error_code(std::errc::operation_canceled));
    }

    // Re-parse URL after potential redirect
    auto cur_parsed = ParsedUrl::parse(current_url);
    if (!cur_parsed) { cancel_timer(); co_return unexpected(cur_parsed.error()); }

    std::string cur_key = FetchClient::make_pool_key(cur_parsed->host, cur_parsed->port);

    // ── Acquire or create connection ─────────────────────────────────────
    std::optional<TcpStream> stream_opt = client_->acquire(cur_key);
    bool reused = stream_opt.has_value();

    if (!stream_opt) {
      // DNS + TCP connect
      auto addr_r = co_await DnsResolver::resolve(cur_parsed->host, cur_parsed->port);
      if (!addr_r) { cancel_timer(); co_return unexpected(addr_r.error()); }

      if (effective_st.stop_requested()) {
        cancel_timer();
        co_return unexpected(std::make_error_code(std::errc::operation_canceled));
      }

      auto conn_r = co_await TcpStream::connect(*addr_r);
      if (!conn_r) { cancel_timer(); co_return unexpected(conn_r.error()); }
      stream_opt = std::move(*conn_r);
    }

    TcpStream &stream = *stream_opt;
    stream.set_nodelay(true);

    // ── Serialise and send request ───────────────────────────────────────
    std::string req_text = detail::serialize_request(
        method_, *cur_parsed, headers_, body_,
        /*keep_alive=*/true);

    std::string_view remaining(req_text);
    bool write_failed = false;
    while (!remaining.empty()) {
      if (effective_st.stop_requested()) {
        cancel_timer();
        co_return unexpected(std::make_error_code(std::errc::operation_canceled));
      }
      auto w = co_await stream.write(
          std::span<const std::byte>(
              reinterpret_cast<const std::byte *>(remaining.data()),
              remaining.size()));
      if (!w) {
        // If we had a reused connection it may have gone stale — retry once
        // with a fresh connection by falling through to re-open.
        if (reused) { reused = false; write_failed = true; break; }
        cancel_timer();
        co_return unexpected(w.error());
      }
      remaining.remove_prefix(*w);
    }

    if (write_failed) {
      // Pooled connection was stale; open a fresh one (next loop iteration
      // will have stream_opt empty because we break before putting it back).
      stream_opt.reset();
      reused = false;
      continue;
    }

    // ── Read response ─────────────────────────────────────────────────────
    auto raw_r = co_await detail::read_http_response(stream, effective_st);
    if (!raw_r) { cancel_timer(); co_return unexpected(raw_r.error()); }

    auto resp_r = detail::parse_response(*raw_r);
    if (!resp_r) { cancel_timer(); co_return resp_r; }

    FetchResponse &resp = *resp_r;

    // ── Connection reuse decision ─────────────────────────────────────────
    std::string_view conn_hdr = resp.header("connection");
    bool server_close = (conn_hdr.find("close") != std::string_view::npos);
    if (!server_close) {
      client_->release(cur_key, std::move(stream));
    }
    // If server_close, TcpStream destructor closes the socket.

    // ── Redirect handling ─────────────────────────────────────────────────
    int status = resp.status();
    bool is_redirect = (status == 301 || status == 302 ||
                        status == 303 || status == 307 || status == 308);
    if (is_redirect && redirects < client_->default_max_redirects()) {
      std::string_view location = resp.header("location");
      if (!location.empty()) {
        current_url = std::string(location);
        ++redirects;
        if (status == 303) method_ = Method::Get;
        continue;
      }
    }

    cancel_timer();
    co_return resp_r;
  }
}

} // namespace qbuem

/** @} */ // end of qbuem_fetch_client
