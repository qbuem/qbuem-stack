#pragma once

/**
 * @file qbuem/core/async_logger.hpp
 * @brief Lock-free single-producer / single-consumer ring-buffer async logger.
 *
 * The reactor thread enqueues log entries without blocking (enqueue is O(1),
 * no mutex, no allocation).  A background flush thread drains the ring buffer
 * and writes to an output stream (default: stderr) in bulk.
 *
 * Usage:
 *   AsyncLogger logger(4096);          // 4096-entry ring buffer
 *   logger.start();                    // spawn background thread
 *
 *   // From reactor thread (hot path):
 *   logger.log("GET", "/path", 200, 123);
 *
 *   // On shutdown:
 *   logger.stop();                     // flush remaining entries and join
 *
 * The AsyncLogger is also compatible with App::set_access_logger():
 *   app.set_access_logger(logger.make_callback());
 */

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace qbuem {

/**
 * @brief Single log entry — fixed-size to avoid heap allocation on the hot path.
 */
struct LogEntry {
  static constexpr size_t kMaxMethod = 8;
  static constexpr size_t kMaxPath   = 256;

  std::array<char, kMaxMethod> method;
  std::array<char, kMaxPath>   path;
  int    status;
  long   duration_us;
  long   timestamp_sec;   // seconds since epoch

  void fill(std::string_view m, std::string_view p, int s, long dur) noexcept {
    size_t ml = std::min(m.size(), kMaxMethod - 1);
    std::memcpy(method.data(), m.data(), ml);
    method[ml] = '\0';

    size_t pl = std::min(p.size(), kMaxPath - 1);
    std::memcpy(path.data(), p.data(), pl);
    path[pl] = '\0';

    status       = s;
    duration_us  = dur;
    timestamp_sec = static_cast<long>(std::time(nullptr));
  }
};

/**
 * @brief Lock-free SPSC ring buffer for log entries.
 *
 * Producer (reactor thread) calls enqueue(); consumer (flush thread) calls
 * dequeue().  Only safe for a single producer and a single consumer.
 * For multi-reactor use, create one AsyncLogger per reactor thread.
 */
/**
 * @brief Output format for AsyncLogger entries.
 */
enum class LogFormat {
  Text, ///< [ISO8601] METHOD /path STATUS Xµs  (default)
  Json, ///< {"ts":"…","method":"…","path":"…","status":N,"duration_us":N}
};

class AsyncLogger {
public:
  /**
   * @param capacity  Ring buffer size (must be a power of two).
   *                  Excess entries are silently dropped if the buffer is full.
   * @param out       Output file (default: stderr).
   * @param fmt       Output format (Text or Json).
   */
  explicit AsyncLogger(size_t capacity = 4096, FILE *out = stderr,
                       LogFormat fmt = LogFormat::Text)
      : mask_(capacity - 1), out_(out), fmt_(fmt), buf_(capacity) {
    // Capacity must be a power of two.
  }

  ~AsyncLogger() { stop(); }

  AsyncLogger(const AsyncLogger &) = delete;
  AsyncLogger &operator=(const AsyncLogger &) = delete;

  /** @brief Spawn the background flush thread. */
  void start() {
    thread_ = std::jthread([this](const std::stop_token& st) { flush_loop(st); });
  }

  /** @brief Flush remaining entries and join the background thread. */
  void stop() {
    if (thread_.joinable()) {
      thread_.request_stop();
      thread_.join();
    }
    // Final drain (called from joining thread).
    drain();
  }

  /**
   * @brief Enqueue a log entry (O(1), no allocation).
   *
   * Safe for concurrent producers (MPSC).  Drops silently if the buffer is
   * full — this is intentional to never block the hot path.
   */
  void log(std::string_view method, std::string_view path,
           int status, long duration_us) noexcept {
    // Spinlock: claim a slot exclusively among concurrent producers.
    while (producer_lock_.test_and_set(std::memory_order_acquire)) {}

    size_t head = head_.load(std::memory_order_relaxed);
    size_t next = (head + 1) & mask_; // wraps at capacity
    if (next == tail_.load(std::memory_order_acquire)) {
      producer_lock_.clear(std::memory_order_release);
      return; // buffer full — drop
    }

    buf_[head & mask_].fill(method, path, status, duration_us);
    head_.store(next, std::memory_order_release);
    producer_lock_.clear(std::memory_order_release);
  }

  /**
   * @brief Return a callback suitable for App::set_access_logger().
   *
   * The returned lambda captures a pointer to this AsyncLogger.
   * The AsyncLogger must outlive the App.
   */
  std::function<void(std::string_view, std::string_view, int, long)>
  make_callback() {
    return [this](std::string_view m, std::string_view p, int s, long us) {
      log(m, p, s, us);
    };
  }

private:
  void flush_loop(const std::stop_token& st) {
    while (!st.stop_requested()) {
      drain();
      // Sleep briefly to yield CPU — 1 ms is a good balance between
      // throughput and latency.
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  // Drain all pending entries and write them to out_.
  void drain() noexcept {
    size_t tail = tail_.load(std::memory_order_relaxed);
    size_t head = head_.load(std::memory_order_acquire);

    while (tail != head) {
      const LogEntry &e = buf_[tail & mask_];
      write_entry(e);
      tail = (tail + 1) & mask_;
    }
    tail_.store(tail, std::memory_order_release);
  }

  // Format and write a single entry.
  void write_entry(const LogEntry &e) {
    std::tm tm{};
    std::time_t ts = static_cast<std::time_t>(e.timestamp_sec);
    gmtime_r(&ts, &tm);

    char ts_buf[24]; // NOLINT(modernize-avoid-c-arrays)
    std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%SZ", &tm);

    std::string line;
    if (fmt_ == LogFormat::Json) {
      // {"ts":"2024-…","method":"GET","path":"/…","status":200,"duration_us":123}
      line = std::format(
          "{{\"ts\":\"{}\",\"method\":\"{}\","
          "\"path\":\"{}\",\"status\":{},\"duration_us\":{}}}\n",
          ts_buf, e.method.data(), e.path.data(), e.status, e.duration_us);
    } else {
      // [YYYY-MM-DDTHH:MM:SSZ] METHOD /path STATUS Xµs
      line = std::format("[{}] {} {} {} {}µs\n",
                         ts_buf, e.method.data(), e.path.data(), e.status, e.duration_us);
    }
    std::fwrite(line.data(), 1, line.size(), out_);
  }

  size_t    mask_;                       // capacity - 1
  FILE     *out_;
  LogFormat fmt_;
  std::vector<LogEntry> buf_;

  alignas(64) std::atomic<size_t> head_{0};        // producer writes
  alignas(64) std::atomic_flag   producer_lock_ = ATOMIC_FLAG_INIT; // MPSC guard
  alignas(64) std::atomic<size_t> tail_{0};        // consumer reads

  std::jthread      thread_;
};

} // namespace qbuem
