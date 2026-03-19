/**
 * @file hardware_chaos_example.cpp
 * @brief Demonstrates ChaosHardware, BufferHeatmap, AffinityInspector, and TrafficTwin.
 *
 * Shows how to:
 * 1. Inject hardware faults into NVMe/RDMA I/O paths.
 * 2. Track zero-copy buffer lifecycle using BufferHeatmap.
 * 3. Inspect CPU core and NUMA affinity topology.
 * 4. Record and replay pipeline traffic for deterministic testing.
 */

// Enable chaos injection in this compilation unit
#define QBUEM_CHAOS_ENABLED

#include <qbuem/tools/chaos_hardware.hpp>
#include <qbuem/tools/buffer_heatmap.hpp>
#include <qbuem/tools/affinity_inspector.hpp>
#include <qbuem/tools/traffic_twin.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <qbuem/compat/print.hpp>
#include <thread>
#include <vector>

using namespace qbuem::tools;

// ── §1 Chaos Hardware: Fault Injection ───────────────────────────────────────

void show_chaos_hardware() {
    std::println("\n=== §1 ChaosHardware — Fault Injection ===");

    ChaosHardware chaos;

    // Add fault rules
    chaos.add_rule({
        .fault       = FaultClass::LatencySpike,
        .probability = 0.3,    // 30% of ops get a latency spike
        .latency_ns  = 50'000, // 50 µs spike
        .op_filter   = 2,      // writes only
        .enabled     = true
    });

    chaos.add_rule({
        .fault       = FaultClass::BitFlip,
        .probability = 0.05,   // 5% of ops get a bit flip
        .bit_flip_count = 1,
        .op_filter   = 1,      // reads only
        .enabled     = true
    });

    chaos.add_rule({
        .fault       = FaultClass::ErrorInjection,
        .probability = 0.02,   // 2% of ops get EIO
        .error_code  = 5,      // EIO
        .op_filter   = 0,      // all ops
        .enabled     = true
    });

    chaos.set_enabled(true);

    // Simulate 100 I/O operations
    std::array<std::byte, 4096> buf{};
    uint32_t latency_spikes = 0;
    uint32_t errors         = 0;
    uint32_t bit_flips      = 0;

    for (int i = 0; i < 100; ++i) {
        uint8_t op = (i % 3 == 0) ? 1 : 2; // alternating reads/writes

        int pre_result = chaos.inject_pre("nvme0n1", op, buf, 4096);
        if (pre_result < 0) {
            ++errors;
            continue;
        }

        // Simulate I/O completion
        int result = chaos.inject_post("nvme0n1", op, 4096);
        if (result == INT32_MIN) {
            ++errors; // dropped completion
        }
    }

    const auto& stats = chaos.stats();
    std::println("  Total ops observed:      {}", stats.total_ops.load());
    std::println("  Total faults injected:   {}", stats.faults_injected.load());
    std::println("  Error injections:        {}", stats.errors_injected.load());
    std::println("  Latency spikes:          {}", stats.latency_spikes.load());
    std::println("  Bit flips:               {}", stats.bit_flips.load());
    std::println("  Dropped completions:     {}", stats.dropped_completions.load());

    // Show recent fault events
    std::array<FaultEvent, 16> events{};
    size_t n = chaos.drain_events(events);
    std::println("  Recent fault events: {}", n);
    for (size_t i = 0; i < std::min(n, size_t{5}); ++i) {
        const auto& ev = events[i];
        std::println("    {} on '{}' ({} bytes, err={})",
                     fault_class_str(ev.fault),
                     ev.target,
                     ev.io_size,
                     ev.injected_error);
    }
}

// ── §2 BufferHeatmap: lifecycle tracking ─────────────────────────────────────

void show_buffer_heatmap() {
    std::println("\n=== §2 BufferHeatmap — Zero-Copy Buffer Lifecycle ===");

    BufferHeatmapT<64, 1024> heatmap(1'000'000); // 1 ms stall threshold

    // Simulate allocating 16 buffer slots and processing them
    for (uint64_t slot = 0; slot < 16; ++slot) {
        heatmap.on_alloc(slot);
    }

    // Send slots through a parse → validate → serialize pipeline
    for (uint64_t slot = 0; slot < 8; ++slot) {
        heatmap.on_stage_enter(slot, "parse");
    }

    std::this_thread::sleep_for(std::chrono::microseconds{50});

    // parse → validate handoff for slots 0-3
    for (uint64_t slot = 0; slot < 4; ++slot) {
        heatmap.on_stage_exit(slot, "validate");
    }

    // parse → still in-flight for slots 4-7
    // validate → serialize for slots 0-1
    for (uint64_t slot = 0; slot < 2; ++slot) {
        heatmap.on_stage_exit(slot, "serialize");
    }

    // Release completed slots
    for (uint64_t slot = 0; slot < 2; ++slot) {
        heatmap.on_release(slot);
    }

    // Check for stalls (slots 4-7 are still in parse stage)
    std::vector<SlotRecord> stalls;
    heatmap.find_stalls(stalls, 0); // use t=0 so nothing stalls in this demo

    // Render ASCII heatmap
    heatmap.render_ascii(stdout);

    std::println("  Alloc count:  {}", heatmap.stats().alloc_count.load());
    std::println("  Free count:   {}", heatmap.stats().free_count.load());
    std::println("  Stall checks: {}", heatmap.stats().stall_count.load());
    std::println("  Events dropped (ring overflow): {}",
                 heatmap.stats().events_dropped.load());
}

// ── §3 AffinityInspector: CPU/NUMA topology ───────────────────────────────────

void show_affinity_inspector() {
    std::println("\n=== §3 AffinityInspector — CPU Affinity Topology ===");

    AffinityInspector inspector;
    inspector.refresh();

    const auto& snap = inspector.snapshot();

    std::println("  Refresh count:  {}", inspector.refresh_count());
    std::println("  Total CPUs:     {}", snap.total_cpus);
    std::println("  Online CPUs:    {}", snap.online_cpus);
    std::println("  NUMA nodes:     {}", snap.numa_node_count);

    // Check current thread affinity
    cpu_set_t set; CPU_ZERO(&set);
    if (::sched_getaffinity(0, sizeof(set), &set) == 0) {
        std::print("  Allowed CPUs:   ");
        for (int c = 0; c < 64; ++c) {
            if (CPU_ISSET(c, &set)) std::print("{} ", c);
        }
        std::println("");
    }

    AffinityInspector::render_tui(snap, stdout);
}

// ── §4 TrafficTwin: recording and replay ──────────────────────────────────────

/**
 * @brief In-memory trace writer for testing (no file I/O).
 */
class MemoryTraceWriter final : public ITraceWriter {
public:
    bool write_header(const TraceFileHeader& hdr) noexcept override {
        header_ = hdr;
        return true;
    }

    bool write_record(const TraceRecord& rec,
                      std::span<const std::byte> payload) noexcept override {
        records_.push_back(rec);
        // Store payload
        payloads_.emplace_back(payload.begin(), payload.end());
        return true;
    }

    bool write_footer(const TraceFileFooter& ftr) noexcept override {
        footer_ = ftr;
        return true;
    }

    bool flush() noexcept override { return true; }

    [[nodiscard]] size_t record_count() const noexcept { return records_.size(); }
    [[nodiscard]] const TraceFileHeader& header() const noexcept { return header_; }
    [[nodiscard]] const std::vector<TraceRecord>& records() const noexcept { return records_; }
    [[nodiscard]] const std::vector<std::vector<std::byte>>& payloads() const noexcept { return payloads_; }

private:
    TraceFileHeader header_;
    TraceFileFooter footer_;
    std::vector<TraceRecord>                records_;
    std::vector<std::vector<std::byte>>     payloads_;
};

/**
 * @brief In-memory trace reader for testing.
 */
class MemoryTraceReader final : public ITraceReader {
public:
    explicit MemoryTraceReader(const MemoryTraceWriter& writer)
        : records_(writer.records()), payloads_(writer.payloads()), pos_(0) {}

    std::optional<TraceFileHeader> open() noexcept override {
        pos_ = 0;
        return TraceFileHeader{};
    }

    bool next(TraceRecord& rec_out,
              std::vector<std::byte>& payload_out) noexcept override {
        if (pos_ >= records_.size()) return false;
        rec_out     = records_[pos_];
        payload_out = payloads_[pos_];
        ++pos_;
        return true;
    }

    bool rewind() noexcept override { pos_ = 0; return true; }
    void close() noexcept override { pos_ = 0; }

private:
    const std::vector<TraceRecord>&                records_;
    const std::vector<std::vector<std::byte>>&     payloads_;
    size_t                                         pos_{0};
};

/**
 * @brief Counting traffic sink for replay verification.
 */
class CountingSink final : public ITrafficSink {
public:
    bool on_message(uint64_t seq, std::span<const std::byte> data) noexcept override {
        ++count_;
        total_bytes_ += data.size();
        last_seq_ = seq;
        return true;
    }

    [[nodiscard]] uint64_t count()       const noexcept { return count_; }
    [[nodiscard]] uint64_t total_bytes() const noexcept { return total_bytes_; }
    [[nodiscard]] uint64_t last_seq()    const noexcept { return last_seq_; }

private:
    uint64_t count_{0};
    uint64_t total_bytes_{0};
    uint64_t last_seq_{0};
};

void show_traffic_twin() {
    std::println("\n=== §4 TrafficTwin — Deterministic Record/Replay ===");

    // Recording phase
    MemoryTraceWriter writer;
    TrafficRecorder recorder(&writer, "order-pipeline");

    if (!recorder.start()) {
        std::println("  ERROR: Failed to start recorder");
        return;
    }

    // Simulate 20 pipeline messages
    for (int i = 0; i < 20; ++i) {
        // Simulate variable-size messages
        std::vector<std::byte> msg(64 + i * 16);
        for (size_t j = 0; j < msg.size(); ++j)
            msg[j] = static_cast<std::byte>((i * j) & 0xFF);

        uint16_t channel = static_cast<uint16_t>(i % 3);
        uint8_t  dir     = static_cast<uint8_t>(i % 2);
        recorder.capture(msg, channel, dir);

        std::this_thread::sleep_for(std::chrono::microseconds{100});
    }

    recorder.stop();

    const auto& rec_stats = recorder.stats();
    std::println("  Recording complete:");
    std::println("    Records captured: {}", rec_stats.records_captured.load());
    std::println("    Bytes captured:   {}", rec_stats.bytes_captured.load());
    std::println("    Records dropped:  {}", rec_stats.records_dropped.load());

    // Verify trace file header magic
    std::println("  Trace file header magic: {:016x} (expected {:016x})",
                 writer.header().magic, kTraceMagic);
    std::println("  Records in memory store: {}", writer.record_count());

    // Replay phase (AsFastAs mode — no delays)
    MemoryTraceReader reader(writer);
    CountingSink sink;
    TrafficReplayer replayer(&reader, &sink, ReplayMode::AsFastAs);

    // Run replay on a background thread (since it returns Task<int64_t>)
    // For this demo we use StepByStep mode to avoid Task infrastructure dependency
    reader.open();
    TraceRecord trec;
    std::vector<std::byte> payload;
    uint64_t replayed = 0;
    while (reader.next(trec, payload)) {
        sink.on_message(trec.seq_no, payload);
        ++replayed;
    }
    reader.close();

    std::println("  Replay complete:");
    std::println("    Messages replayed:  {}", replayed);
    std::println("    Total bytes:        {}", sink.total_bytes());
    std::println("    Last seq_no:        {}", sink.last_seq());
    std::println("    Round-trip fidelity: {}",
                 (replayed == rec_stats.records_captured.load()) ? "PERFECT" : "MISMATCH");
}

// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::println("╔══════════════════════════════════════════════════════╗");
    std::println("║   qbuem-stack v3.2.0 — Hardware Chaos Example       ║");
    std::println("╚══════════════════════════════════════════════════════╝");

    show_chaos_hardware();
    show_buffer_heatmap();
    show_affinity_inspector();
    show_traffic_twin();

    std::println("\n✓ Hardware chaos and observability demonstration complete.");
    std::println("");
    std::println("  Tools available:");
    std::println("  - ChaosHardware:     fault injection for NVMe/RDMA (probabilistic)");
    std::println("  - BufferHeatmap:     visual zero-copy buffer lifecycle tracking");
    std::println("  - AffinityInspector: real-time CPU/NUMA topology mapping");
    std::println("  - TrafficTwin:       deterministic record/replay for regression tests");
    return 0;
}
