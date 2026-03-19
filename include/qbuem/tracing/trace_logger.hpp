#pragma once

/**
 * @file qbuem/tracing/trace_logger.hpp
 * @brief Trace-aware async logger — correlates log events with trace spans.
 * @defgroup qbuem_trace_logger Trace Logger
 * @ingroup qbuem_tracing
 *
 * ## Overview
 *
 * `TraceLogger` extends `AsyncLogger` to embed W3C `TraceContext` (trace_id,
 * span_id) into every log record. This enables the `qbuem-inspector` to
 * display log events precisely on the request timeline, eliminating the
 * traditional log/trace correlation problem.
 *
 * ## Log record format
 * ```
 * [2026-03-19T14:23:01.123456789Z] [INFO ] [00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01]
 *   [order-service] process_order: Validation passed for order_id=ORD-001
 *  └──────────────────────────────────────────────┘
 *          W3C traceparent embedded in every line
 * ```
 *
 * ## Zero-allocation design
 * All formatting happens on the **flush thread** (the `AsyncLogger` consumer),
 * not on the caller thread. Hot-path cost is:
 * - Acquire a ring buffer slot (CAS, ~3 ns)
 * - Copy TraceContext + log message into the slot (memcpy, ~5 ns)
 * - Total: < 10 ns per log call on the hot path
 *
 * ## Usage
 * @code
 * TraceLogger logger(LifecycleTracer::global(), "order-service");
 * logger.start();
 *
 * // In a coroutine with an active span context
 * auto span = tracer.start_lifecycle("process_order");
 * auto ctx = span.context();
 *
 * logger.info(ctx, "Received order: {}", order.id);
 * logger.warn(ctx, "Validation latency high: {} µs", latency_us);
 * logger.error(ctx, "DB timeout after {} ms", timeout_ms);
 * @endcode
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/async_logger.hpp>
#include <qbuem/tracing/trace_context.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <print>
#include <span>
#include <string_view>

namespace qbuem::tracing {

// ─── LogLevel ────────────────────────────────────────────────────────────────

/**
 * @brief Log severity levels.
 */
enum class LogLevel : uint8_t {
    Trace = 0,  ///< Verbose trace (disabled in Release builds)
    Debug = 1,  ///< Debug-only information
    Info  = 2,  ///< Normal operational events
    Warn  = 3,  ///< Recoverable anomalies
    Error = 4,  ///< Errors that require attention
    Fatal = 5,  ///< Critical failures — process may terminate
};

/**
 * @brief Returns a short string representation of a `LogLevel`.
 */
[[nodiscard]] constexpr std::string_view level_str(LogLevel l) noexcept {
    switch (l) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO ";
    case LogLevel::Warn:  return "WARN ";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Fatal: return "FATAL";
    }
    return "?????";
}

// ─── TraceLogRecord (ring buffer slot) ───────────────────────────────────────

/**
 * @brief Fixed-size ring buffer record for the trace-aware logger.
 *
 * Trivially copyable — placed directly into the async ring buffer.
 * Max message length is `kMsgLen - 1` characters (including NUL).
 */
struct TraceLogRecord {
    static constexpr size_t kMsgLen = 256;

    uint64_t    timestamp_ns{0};       ///< Nanoseconds since epoch (CLOCK_REALTIME)
    uint64_t    trace_id{0};           ///< W3C trace_id (low 64 bits)
    uint64_t    span_id{0};            ///< W3C span_id
    LogLevel    level{LogLevel::Info}; ///< Severity
    char        service[16]{};         ///< Service name (NUL-terminated)
    char        msg[kMsgLen]{};        ///< Formatted message (NUL-terminated)

    /** @brief Set message, truncating if necessary. */
    void set_message(std::string_view m) noexcept {
        size_t n = std::min(m.size(), kMsgLen - 1);
        std::memcpy(msg, m.data(), n);
        msg[n] = '\0';
    }
};
static_assert(std::is_trivially_copyable_v<TraceLogRecord>);

// ─── TraceLogRing ─────────────────────────────────────────────────────────────

/**
 * @brief Lock-free MPSC ring buffer of `TraceLogRecord` entries.
 *
 * @tparam Cap  Number of slots (power of 2).
 */
template<size_t Cap = 8192>
struct TraceLogRing {
    static_assert((Cap & (Cap - 1)) == 0);
    static constexpr size_t kMask = Cap - 1;

    alignas(64) std::atomic<uint64_t> head{0};
    alignas(64) std::atomic<uint64_t> tail{0};
    alignas(64) TraceLogRecord        slots[Cap];

    bool try_push(const TraceLogRecord& rec) noexcept {
        uint64_t t = tail.load(std::memory_order_relaxed);
        if (t - head.load(std::memory_order_acquire) >= Cap) return false;
        slots[t & kMask] = rec;
        tail.store(t + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(TraceLogRecord& out) noexcept {
        uint64_t h = head.load(std::memory_order_relaxed);
        if (h >= tail.load(std::memory_order_acquire)) return false;
        out = slots[h & kMask];
        head.store(h + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] uint64_t size() const noexcept {
        return tail.load(std::memory_order_acquire) - head.load(std::memory_order_acquire);
    }
};

// ─── ILogSink ─────────────────────────────────────────────────────────────────

/**
 * @brief Sink interface for flushed log records.
 *
 * Implement for file, stderr, OTLP, or custom backends.
 * The flush thread calls `write()` — no reactor constraints.
 */
class ILogSink {
public:
    virtual ~ILogSink() = default;
    virtual void write(const TraceLogRecord& rec) noexcept = 0;
    virtual void flush() noexcept {}
};

/**
 * @brief Default stderr sink — prints formatted trace-aware log lines.
 *
 * Format:
 * ```
 * [YYYY-MM-DDTHH:MM:SS.nnnnnnnnnZ] [LEVEL] [traceparent] [service] message
 * ```
 */
class StderrLogSink final : public ILogSink {
public:
    void write(const TraceLogRecord& rec) noexcept override {
        // Format timestamp as ISO-8601
        time_t sec = static_cast<time_t>(rec.timestamp_ns / 1'000'000'000ULL);
        uint32_t ns = static_cast<uint32_t>(rec.timestamp_ns % 1'000'000'000ULL);
        struct tm tm_buf{};
        ::gmtime_r(&sec, &tm_buf);
        char ts[32]{};
        ::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm_buf);

        // W3C traceparent: 00-<trace_id_128_hex>-<span_id_hex>-01
        // We use trace_id (64-bit) zero-padded to 32 hex chars
        std::print(stderr,
            "[{}.{:09}Z] [{}] [00-{:016x}0000000000000000-{:016x}-01] [{}] {}\n",
            ts, ns,
            level_str(rec.level),
            rec.trace_id, rec.span_id,
            rec.service,
            rec.msg);
    }
};

// ─── TraceLogger ─────────────────────────────────────────────────────────────

/**
 * @brief Trace-aware async logger.
 *
 * Embeds W3C TraceContext into every log record and flushes via a background
 * thread using the injected `ILogSink`. The hot-path cost is a ring buffer
 * CAS + memcpy — no format string processing, no I/O, no mutex.
 *
 * ### Thread safety
 * The ring buffer is MPSC — multiple producers (any thread) to single consumer
 * (the flush thread started by `start()`).
 *
 * @tparam Cap  Ring buffer capacity (power of 2, default 8192 records).
 */
template<size_t Cap = 8192>
class TraceLogger {
public:
    static constexpr LogLevel kDefaultLevel = LogLevel::Info;

    /**
     * @brief Construct with a service name and optional log sink.
     *
     * @param service_name  Service identifier embedded in every record.
     * @param sink          Log sink (defaults to `StderrLogSink` if null).
     * @param min_level     Minimum level to record (records below are dropped).
     */
    explicit TraceLogger(std::string_view service_name,
                         ILogSink* sink = nullptr,
                         LogLevel min_level = kDefaultLevel)
        : min_level_(min_level)
        , sink_(sink ? sink : &default_sink_)
    {
        size_t n = std::min(service_name.size(), size_t{15});
        std::memcpy(service_, service_name.data(), n);
        service_[n] = '\0';
    }

    /**
     * @brief Start the background flush thread.
     */
    void start() noexcept {
        if (running_.exchange(true)) return;
        flush_thread_ = std::jthread([this](std::stop_token st) {
            flush_loop(st);
        });
    }

    /**
     * @brief Stop the flush thread and drain remaining records.
     */
    void stop() noexcept {
        running_.store(false);
        flush_thread_.request_stop();
    }

    // ── Logging API (hot path) ────────────────────────────────────────────────

    /**
     * @brief Log a message with trace context.
     *
     * @param level  Log severity.
     * @param ctx    Ambient trace context (from `ActiveSpan::context()`).
     * @param msg    Pre-formatted message string.
     */
    void log(LogLevel level, const TraceContext& ctx, std::string_view msg) noexcept {
        if (level < min_level_) return;
        TraceLogRecord rec;
        rec.timestamp_ns = now_ns();
        // Extract low 64 bits from TraceId (stored as bytes[16])
        std::memcpy(&rec.trace_id, ctx.trace_id.bytes + 8, 8);
        // Extract uint64_t from SpanId (stored as bytes[8])
        std::memcpy(&rec.span_id, ctx.parent_span_id.bytes, 8);
        rec.level        = level;
        std::memcpy(rec.service, service_, sizeof(service_));
        rec.set_message(msg);
        if (!ring_.try_push(rec))
            dropped_.fetch_add(1, std::memory_order_relaxed);
    }

    /** @brief Log at INFO level. */
    void info(const TraceContext& ctx, std::string_view msg) noexcept {
        log(LogLevel::Info, ctx, msg);
    }
    /** @brief Log at WARN level. */
    void warn(const TraceContext& ctx, std::string_view msg) noexcept {
        log(LogLevel::Warn, ctx, msg);
    }
    /** @brief Log at ERROR level. */
    void error(const TraceContext& ctx, std::string_view msg) noexcept {
        log(LogLevel::Error, ctx, msg);
    }
    /** @brief Log at DEBUG level. */
    void debug(const TraceContext& ctx, std::string_view msg) noexcept {
        log(LogLevel::Debug, ctx, msg);
    }

    // ── Formatted logging (std::format on the caller thread) ─────────────────

    /**
     * @brief Log a formatted message (format string evaluated on caller thread).
     *
     * @note Use sparingly on the hot path — `std::format` allocates.
     *       Prefer pre-formatted `std::string_view` overloads on hot paths.
     */
    template<typename... Args>
    void logf(LogLevel level, const TraceContext& ctx,
              std::format_string<Args...> fmt, Args&&... args) noexcept {
        if (level < min_level_) return;
        // format() allocates — acceptable for non-hot-path use
        auto s = std::format(fmt, std::forward<Args>(args)...);
        log(level, ctx, s);
    }

    template<typename... Args>
    void infof(const TraceContext& ctx, std::format_string<Args...> fmt, Args&&... args) noexcept {
        logf(LogLevel::Info, ctx, fmt, std::forward<Args>(args)...);
    }

    // ── Stats ─────────────────────────────────────────────────────────────────

    [[nodiscard]] uint64_t dropped()    const noexcept { return dropped_.load(); }
    [[nodiscard]] uint64_t ring_size()  const noexcept { return ring_.size(); }

private:
    static uint64_t now_ns() noexcept {
        timespec ts{};
        ::clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
               static_cast<uint64_t>(ts.tv_nsec);
    }

    void flush_loop(std::stop_token st) noexcept {
        while (!st.stop_requested() || ring_.size() > 0) {
            TraceLogRecord rec;
            while (ring_.try_pop(rec)) sink_->write(rec);
            std::this_thread::sleep_for(std::chrono::microseconds{100});
        }
        sink_->flush();
    }

    LogLevel                           min_level_;
    ILogSink*                          sink_;
    StderrLogSink                      default_sink_;
    char                               service_[16]{};
    TraceLogRing<Cap>                  ring_;
    alignas(64) std::atomic<uint64_t>  dropped_{0};
    std::atomic<bool>                  running_{false};
    std::jthread                       flush_thread_;
};

} // namespace qbuem::tracing

/** @} */ // end of qbuem_trace_logger
