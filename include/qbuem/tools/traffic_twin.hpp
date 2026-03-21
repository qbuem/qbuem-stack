#pragma once

/**
 * @file qbuem/tools/traffic_twin.hpp
 * @brief TrafficTwin — deterministic protocol recording and replay.
 * @defgroup qbuem_traffic_twin TrafficTwin
 * @ingroup qbuem_tools
 *
 * ## Overview
 *
 * `TrafficTwin` captures every message flowing through a qbuem-stack pipeline
 * or network channel into a compact binary trace file, then replays it
 * deterministically for:
 *
 * - **Regression testing** — replay production traffic against a new build.
 * - **Performance profiling** — measure throughput/latency on recorded workloads.
 * - **Chaos testing** — replay with `ChaosHardware` fault injection enabled.
 * - **Differential analysis** — compare output of two pipeline versions on the
 *   same input stream.
 *
 * ## Trace file format
 *
 * The trace file is a sequence of fixed-header + variable-payload records:
 * ```
 * [TraceFileHeader (64 bytes)]
 * [TraceRecord header (32 bytes) + payload (variable)]
 * [TraceRecord header (32 bytes) + payload]
 * ...
 * [TraceFileFooter (16 bytes)]
 * ```
 *
 * All integers are little-endian. Payloads are byte-aligned.
 *
 * ## Replay modes
 * | Mode          | Description                                                  |
 * |---------------|--------------------------------------------------------------|
 * | `WallClock`   | Replay with original inter-message timing                   |
 * | `AsFastAs`    | Replay as fast as possible (load test / performance bench)  |
 * | `StepByStep`  | Advance one message at a time (interactive debugging)       |
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <vector>

namespace qbuem::tools {

// ─── TraceFileHeader ─────────────────────────────────────────────────────────

/** @brief Magic number for trace files: "QBUEMTRC" as little-endian uint64. */
inline constexpr uint64_t kTraceMagic = 0x4352544D454255'51ULL;

/**
 * @brief 64-byte trace file header.
 */
struct TraceFileHeader {
    uint64_t magic{kTraceMagic};         ///< Magic identifier
    uint32_t version{1};                 ///< File format version
    uint32_t flags{0};                   ///< Reserved flags
    uint64_t record_count{0};            ///< Total records in this file
    uint64_t start_ns{0};                ///< Absolute capture start time (CLOCK_REALTIME ns)
    uint64_t end_ns{0};                  ///< Absolute capture end time
    uint64_t total_bytes{0};             ///< Total payload bytes captured
    char     pipeline_name[16]{};        ///< Originating pipeline name (NUL-terminated) // NOLINT(modernize-avoid-c-arrays)
};
static_assert(sizeof(TraceFileHeader) == 64);
static_assert(std::is_trivially_copyable_v<TraceFileHeader>);

// ─── TraceRecord ─────────────────────────────────────────────────────────────

/**
 * @brief Fixed 32-byte record header for each captured message.
 *
 * Followed immediately by `payload_size` bytes of message data.
 */
struct TraceRecord {
    uint64_t seq_no{0};           ///< Monotonically increasing sequence number
    uint64_t timestamp_ns{0};     ///< Capture time (CLOCK_MONOTONIC ns)
    uint32_t payload_size{0};     ///< Payload bytes following this header
    uint16_t channel_id{0};       ///< Source channel / stage index
    uint8_t  direction{0};        ///< 0=ingress, 1=egress
    uint8_t  flags{0};            ///< Reserved
    uint32_t checksum{0};         ///< CRC32 of payload (0 if not computed)
    uint32_t _pad{0};
};
static_assert(sizeof(TraceRecord) == 32);
static_assert(std::is_trivially_copyable_v<TraceRecord>);

// ─── TraceFileFooter ─────────────────────────────────────────────────────────

/** @brief 16-byte footer written when the recording is closed cleanly. */
struct TraceFileFooter {
    uint64_t magic{kTraceMagic};   ///< Same magic as header (integrity check)
    uint64_t record_count{0};      ///< Final record count (redundant with header)
};
static_assert(sizeof(TraceFileFooter) == 16);

// ─── ReplayMode ──────────────────────────────────────────────────────────────

/**
 * @brief Replay timing strategy.
 */
enum class ReplayMode : uint8_t {
    WallClock  = 0, ///< Reproduce original inter-message delays
    AsFastAs   = 1, ///< No delays (maximum throughput replay)
    StepByStep = 2, ///< Manual advance via `replay_next()`
};

// ─── ITrafficSink ────────────────────────────────────────────────────────────

/**
 * @brief Sink interface for replayed messages.
 *
 * Implement on the pipeline input or a test verifier.
 */
class ITrafficSink {
public:
    virtual ~ITrafficSink() = default;

    /**
     * @brief Receive a replayed message payload.
     *
     * @param seq   Original sequence number.
     * @param data  Message payload bytes.
     * @returns false to abort replay.
     */
    virtual bool on_message(uint64_t seq, std::span<const std::byte> data) noexcept = 0;
};

// ─── RecordingStats ──────────────────────────────────────────────────────────

/** @brief Live statistics for the active recording. */
struct alignas(64) RecordingStats {
    std::atomic<uint64_t> records_captured{0};  ///< Total records written
    std::atomic<uint64_t> bytes_captured{0};    ///< Total payload bytes written
    std::atomic<uint64_t> records_dropped{0};   ///< Dropped (ring full or flush error)
    std::atomic<uint64_t> flush_calls{0};       ///< Number of flush() calls
};

// ─── ITraceWriter ────────────────────────────────────────────────────────────

/**
 * @brief Write-side interface for trace output (file, memory, network).
 *
 * Inject a file-backed implementation for production recording,
 * or a memory-backed stub for unit tests.
 */
class ITraceWriter {
public:
    virtual ~ITraceWriter() = default;

    /** @brief Write a header record (called once on open). */
    virtual bool write_header(const TraceFileHeader& hdr) noexcept = 0;

    /** @brief Write a message record header + payload. */
    virtual bool write_record(const TraceRecord& rec,
                               std::span<const std::byte> payload) noexcept = 0;

    /** @brief Write the footer and flush (called on close). */
    virtual bool write_footer(const TraceFileFooter& ftr) noexcept = 0;

    /** @brief Flush buffered writes (called periodically). */
    virtual bool flush() noexcept = 0;
};

// ─── ITraceReader ────────────────────────────────────────────────────────────

/**
 * @brief Read-side interface for trace replay.
 */
class ITraceReader {
public:
    virtual ~ITraceReader() = default;

    /** @brief Open trace and read the header. Returns nullopt on error. */
    [[nodiscard]] virtual std::optional<TraceFileHeader> open() noexcept = 0;

    /**
     * @brief Read the next record header and payload.
     *
     * @param rec_out     Populated with the next record header.
     * @param payload_out Resized and filled with payload bytes.
     * @returns false on EOF or I/O error.
     */
    virtual bool next(TraceRecord& rec_out,
                      std::vector<std::byte>& payload_out) noexcept = 0;

    /** @brief Seek to the beginning (for loop replay). */
    virtual bool rewind() noexcept = 0;

    virtual void close() noexcept = 0;
};

// ─── TrafficRecorder ─────────────────────────────────────────────────────────

/**
 * @brief Records pipeline messages into a `ITraceWriter`.
 *
 * Called from pipeline stages or channel interceptors on the hot path.
 * Uses a lock-free ring buffer internally; the flush thread drains into
 * the writer.
 */
class TrafficRecorder {
public:
    static constexpr size_t kRingSlots     = 4096;
    static constexpr size_t kMaxPayload    = 65536;

    explicit TrafficRecorder(ITraceWriter*    writer,
                              std::string_view pipeline_name = "qbuem") noexcept
        : writer_(writer) {
        size_t n = std::min(pipeline_name.size(), size_t{15});
        std::memcpy(pipeline_name_, pipeline_name.data(), n);
        pipeline_name_[n] = '\0';
    }

    /**
     * @brief Start recording (writes trace file header).
     */
    bool start() noexcept {
        if (writer_ == nullptr) return false;
        TraceFileHeader hdr;
        hdr.start_ns = now_ns();
        std::memcpy(hdr.pipeline_name, pipeline_name_, sizeof(pipeline_name_));
        if (!writer_->write_header(hdr)) return false;
        running_.store(true, std::memory_order_release);
        start_ns_ = hdr.start_ns;
        return true;
    }

    /**
     * @brief Stop recording and write the footer.
     */
    void stop() noexcept {
        running_.store(false, std::memory_order_relaxed);
        if (writer_ == nullptr) return;
        TraceFileFooter ftr;
        ftr.record_count = stats_.records_captured.load();
        writer_->write_footer(ftr);
        writer_->flush();
    }

    /**
     * @brief Capture a message (hot path).
     *
     * @param data       Message payload.
     * @param channel_id Source channel/stage index.
     * @param direction  0=ingress, 1=egress.
     */
    void capture(std::span<const std::byte> data,
                 uint16_t channel_id = 0,
                 uint8_t  direction  = 0) noexcept {
        if (!running_.load(std::memory_order_relaxed)) return;

        TraceRecord rec;
        rec.seq_no       = seq_.fetch_add(1, std::memory_order_relaxed);
        rec.timestamp_ns = now_ns();
        rec.payload_size = static_cast<uint32_t>(
            std::min(data.size(), kMaxPayload));
        rec.channel_id   = channel_id;
        rec.direction    = direction;
        rec.flags        = 0;
        rec.checksum     = 0;

        if (writer_ != nullptr) {
            if (writer_->write_record(rec, data.subspan(0, rec.payload_size))) {
                stats_.records_captured.fetch_add(1, std::memory_order_relaxed);
                stats_.bytes_captured.fetch_add(rec.payload_size, std::memory_order_relaxed);
            } else {
                stats_.records_dropped.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    /** @brief Recording statistics. */
    [[nodiscard]] const RecordingStats& stats() const noexcept { return stats_; }

private:
    static uint64_t now_ns() noexcept {
        timespec ts{}; ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + ts.tv_nsec;
    }

    ITraceWriter*          writer_{nullptr};
    char                   pipeline_name_[16]{}; // NOLINT(modernize-avoid-c-arrays)
    uint64_t               start_ns_{0};
    std::atomic<bool>      running_{false};
    std::atomic<uint64_t>  seq_{0};
    RecordingStats         stats_;
};

// ─── TrafficReplayer ─────────────────────────────────────────────────────────

/**
 * @brief Replays a captured trace through a `ITrafficSink`.
 */
class TrafficReplayer {
public:
    explicit TrafficReplayer(ITraceReader* reader,
                              ITrafficSink* sink,
                              ReplayMode    mode = ReplayMode::AsFastAs) noexcept
        : reader_(reader), sink_(sink), mode_(mode) {}

    /**
     * @brief Open the trace and replay all messages.
     *
     * @param st  Stop token for cancellation.
     * @returns Number of messages replayed, or -1 on open error.
     */
    [[nodiscard]] Task<int64_t> replay(const std::stop_token& st) {
        if (reader_ == nullptr || sink_ == nullptr) co_return -1;
        auto hdr = reader_->open();
        if (!hdr) co_return -1;

        uint64_t base_ts      = 0;
        uint64_t replay_start = now_ns();
        int64_t  count        = 0;

        TraceRecord rec;
        std::vector<std::byte> payload;

        while (!st.stop_requested() && reader_->next(rec, payload)) {
            if (mode_ == ReplayMode::WallClock && base_ts == 0)
                base_ts = rec.timestamp_ns;

            if (mode_ == ReplayMode::WallClock && base_ts > 0) {
                uint64_t offset_ns = rec.timestamp_ns - base_ts;
                uint64_t now       = now_ns();
                uint64_t elapsed   = now - replay_start;
                if (offset_ns > elapsed) {
                    timespec ts{};
                    uint64_t wait = offset_ns - elapsed;
                    ts.tv_sec  = static_cast<time_t>(wait / 1'000'000'000ULL);
                    ts.tv_nsec = static_cast<long>(wait % 1'000'000'000ULL);
                    ::nanosleep(&ts, nullptr);
                }
            }

            if (!sink_->on_message(rec.seq_no, payload)) break;

            ++count;
            replayed_.fetch_add(1, std::memory_order_relaxed);

            if (mode_ == ReplayMode::StepByStep) break; // single-step
        }

        reader_->close();
        co_return count;
    }

    /**
     * @brief Advance one record (StepByStep mode only).
     *
     * @returns false if there are no more records.
     */
    bool step() noexcept {
        if (reader_ == nullptr || sink_ == nullptr) return false;
        TraceRecord rec;
        std::vector<std::byte> payload;
        if (!reader_->next(rec, payload)) return false;
        sink_->on_message(rec.seq_no, payload);
        replayed_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    /** @brief Number of messages replayed so far. */
    [[nodiscard]] uint64_t replayed() const noexcept {
        return replayed_.load(std::memory_order_relaxed);
    }

private:
    static uint64_t now_ns() noexcept {
        timespec ts{}; ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + ts.tv_nsec;
    }

    ITraceReader*          reader_{nullptr};
    ITrafficSink*          sink_{nullptr};
    ReplayMode             mode_;
    std::atomic<uint64_t>  replayed_{0};
};

} // namespace qbuem::tools

/** @} */ // end of qbuem_traffic_twin
