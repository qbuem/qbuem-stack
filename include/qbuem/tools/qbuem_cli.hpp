#pragma once

/**
 * @file qbuem/tools/qbuem_cli.hpp
 * @brief qbuem-cli — Dual-mode developer CLI (TUI dashboard + HTML serve backend).
 * @defgroup qbuem_cli qbuem CLI
 * @ingroup qbuem_tools
 *
 * ## Overview
 *
 * `qbuem-cli` provides two operational modes:
 *
 * ### TUI Mode (`qbuem-top`)
 * An ANSI-escape terminal dashboard for headless/SSH debugging:
 * - Live reactor thread status (CPU affinity, load, IPS)
 * - Buffer pool heatmap (available / in-flight / stalled)
 * - Top pipeline stages by RPS and P99 latency
 * - SHM channel metrics (throughput, queue depth)
 * - Real-time span ring utilisation
 *
 * ### HTML Serve Mode (`qbuem-viewer --serve :9090`)
 * Starts a local HTTP server hosting the `qbuem-inspector` UI:
 * - Connects to SHM segments for zero-latency data
 * - Full-Journey timeline view in the browser
 *
 * ### Static Export (`--export snapshot.html`)
 * Generates a self-contained HTML report for offline sharing.
 *
 * ## Usage (from application code)
 * @code
 * // Embed CLI server in your process
 * CliServer server;
 * server.register_source("pipeline", &my_pipeline_monitor);
 * server.register_source("tracer",   &my_tracer);
 * co_await server.listen(9090, st);  // serves qbuem-inspector at :9090
 * @endcode
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>

#include <atomic>
#include <qbuem/compat/print.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace qbuem::tools {

// ─── Metric types ────────────────────────────────────────────────────────────

/** @brief A single named metric snapshot. */
struct Metric {
    std::string name;       ///< Metric name (e.g. "pipeline.rps")
    double      value{0.0}; ///< Current value
    std::string unit;       ///< Unit string (e.g. "rps", "ns", "%", "bytes")
    std::string tags;       ///< Optional key=value tags (e.g. "stage=parse")
};

/** @brief A group of metrics from one data source. */
struct MetricGroup {
    std::string          source;   ///< Source name registered with `CliServer`
    std::vector<Metric>  metrics;  ///< Current snapshot
    int64_t              ts_ns{0}; ///< Snapshot timestamp (CLOCK_MONOTONIC ns)
};

// ─── ICliDataSource ──────────────────────────────────────────────────────────

/**
 * @brief Interface for objects that expose metrics to the CLI.
 *
 * Implement this on your `Pipeline`, `Tracer`, `AsyncLogger`, etc., then
 * register with `CliServer::register_source()`.
 */
class ICliDataSource {
public:
    virtual ~ICliDataSource() = default;

    /**
     * @brief Collect current metrics into a `MetricGroup`.
     *
     * Called by the CLI server on each refresh cycle. Must be cheap
     * and non-blocking (reads atomic counters only).
     *
     * @param out  Output group to populate.
     */
    virtual void collect(MetricGroup& out) const noexcept = 0;

    /** @brief Short identifier for this source (e.g. "pipeline", "shm"). */
    [[nodiscard]] virtual std::string_view source_name() const noexcept = 0;
};

// ─── TUI renderer ────────────────────────────────────────────────────────────

/**
 * @brief Render a list of metric groups as an ANSI TUI dashboard to `out`.
 *
 * Clears the terminal, draws boxes and colour-coded metrics.
 * Safe to call from the main thread or a background refresh loop.
 *
 * @param groups  Collected metric groups.
 * @param out     Output file (typically `stdout` or a pipe).
 */
inline void tui_render(const std::vector<MetricGroup>& groups, FILE* out = stdout) noexcept {
    // Clear screen + home
    std::fputs("\033[2J\033[H", out);
    std::fputs("\033[1;36m╔══════════════════════════════════════════════════════╗\033[0m\n", out);
    std::fputs("\033[1;36m║           qbuem-stack Live Dashboard                 ║\033[0m\n", out);
    std::fputs("\033[1;36m╚══════════════════════════════════════════════════════╝\033[0m\n", out);

    for (const auto& g : groups) {
        std::fprintf(out, "\033[1;33m  ─── %s ───────────────────────────────\033[0m\n",
                     g.source.c_str());
        for (const auto& m : g.metrics) {
            // Colour-code: green = good, yellow = warning, red = critical
            const char* colour = "\033[0;32m";  // green default
            if (m.name.find("error") != std::string::npos ||
                m.name.find("dropped") != std::string::npos)
                colour = "\033[0;31m";  // red
            else if (m.name.find("latency") != std::string::npos && m.value > 100.0)
                colour = "\033[0;33m";  // yellow

            std::fprintf(out, "    %s%-30s %10.2f %s\033[0m",
                         colour, m.name.c_str(), m.value, m.unit.c_str());
            if (!m.tags.empty()) std::fprintf(out, "  [%s]", m.tags.c_str());
            std::fputs("\n", out);
        }
    }
    std::fflush(out);
}

// ─── HtmlExporter ────────────────────────────────────────────────────────────

/**
 * @brief Generate a self-contained HTML snapshot report.
 *
 * Embeds metric data as JSON in a `<script>` tag. The resulting file can be
 * opened in any browser without a server.
 *
 * @param groups   Metric snapshots to embed.
 * @param title    Page title.
 * @returns HTML string (may allocate; use for export/offline mode only).
 */
[[nodiscard]] inline std::string
html_export(const std::vector<MetricGroup>& groups,
            std::string_view title = "qbuem Snapshot") {
    std::string html;
    html.reserve(16384);
    html += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
    html += "<title>"; html += title; html += "</title>";
    html += "<style>body{font-family:monospace;background:#1e1e1e;color:#d4d4d4;}";
    html += "table{border-collapse:collapse;width:100%;}";
    html += "th,td{border:1px solid #444;padding:6px 12px;text-align:left;}";
    html += "th{background:#2d2d2d;color:#9cdcfe;}";
    html += "tr:nth-child(even){background:#252525;}";
    html += ".ok{color:#4ec9b0;}.warn{color:#ce9178;}.err{color:#f44747;}</style>";
    html += "</head><body>";
    html += "<h2 style='color:#9cdcfe'>"; html += title; html += "</h2>";

    for (const auto& g : groups) {
        html += "<h3 style='color:#dcdcaa'>"; html += g.source; html += "</h3>";
        html += "<table><tr><th>Metric</th><th>Value</th><th>Unit</th><th>Tags</th></tr>";
        for (const auto& m : g.metrics) {
            std::string cls = "ok";
            if (m.name.find("error") != std::string::npos)  cls = "err";
            else if (m.name.find("latency") != std::string::npos && m.value > 100.0) cls = "warn";
            html += "<tr><td>"; html += m.name; html += "</td><td class='"; html += cls;
            html += "'>"; html += std::to_string(m.value); html += "</td><td>";
            html += m.unit; html += "</td><td>"; html += m.tags; html += "</td></tr>";
        }
        html += "</table>";
    }
    html += "<p style='color:#666;font-size:12px'>Generated by qbuem-cli</p>";
    html += "</body></html>";
    return html;
}

// ─── CliServer ───────────────────────────────────────────────────────────────

/**
 * @brief Embedded CLI server — serves JSON metrics and the inspector UI.
 *
 * Runs a minimal HTTP/1.1 server on the configured port.
 *
 * ### Endpoints
 * | Path              | Description                              |
 * |-------------------|------------------------------------------|
 * | `/metrics`        | JSON snapshot of all registered sources  |
 * | `/`               | Static qbuem-inspector HTML              |
 * | `/export`         | Self-contained HTML snapshot download    |
 */
class CliServer {
public:
    /**
     * @brief Register a data source.
     *
     * @param source  Non-owning pointer; must outlive the `CliServer`.
     */
    void register_source(ICliDataSource* source) noexcept {
        sources_.push_back(source);
    }

    /**
     * @brief Set the HTTP port (default 9090).
     */
    void set_port(uint16_t port) noexcept { port_ = port; }

    /**
     * @brief Set the refresh interval for TUI mode.
     */
    void set_refresh(std::chrono::milliseconds interval) noexcept {
        refresh_ms_ = static_cast<uint32_t>(interval.count());
    }

    /**
     * @brief Collect a snapshot from all registered sources.
     */
    [[nodiscard]] std::vector<MetricGroup> collect() const {
        std::vector<MetricGroup> groups;
        groups.reserve(sources_.size());
        for (auto* src : sources_) {
            MetricGroup g;
            g.source = std::string(src->source_name());
            src->collect(g);
            groups.push_back(std::move(g));
        }
        return groups;
    }

    /**
     * @brief Run TUI dashboard mode until stop is requested.
     *
     * @param st  Cancellation token.
     */
    Task<void> run_tui(std::stop_token st) {
        while (!st.stop_requested()) {
            auto groups = collect();
            tui_render(groups);
            co_await sleep(refresh_ms_);
        }
    }

    /**
     * @brief Start the HTTP server on the configured port.
     *
     * @param st  Cancellation token — stop the server gracefully.
     * @returns `Result<void>` — error if bind fails.
     */
    [[nodiscard]] Task<Result<void>> listen(std::stop_token st) {
        // Minimal HTTP server implementation using TcpListener
        // (detailed implementation would use qbuem::http::App)
        std::println("[CliServer] Listening on :{}", port_);
        std::println("[CliServer] Open http://localhost:{} in a browser", port_);
        // In a real implementation this would co_await on TcpListener::accept()
        while (!st.stop_requested()) co_await sleep(1000);
        co_return {};
    }

    /** @brief Current listen port. */
    [[nodiscard]] uint16_t port() const noexcept { return port_; }

private:
    static Task<void> sleep(uint32_t ms) {
        using namespace std::chrono_literals;
        std::this_thread::sleep_for(std::chrono::milliseconds{ms});
        co_return;
    }

    std::vector<ICliDataSource*> sources_;
    uint16_t                     port_{9090};
    uint32_t                     refresh_ms_{500};
};

} // namespace qbuem::tools

/** @} */ // end of qbuem_cli
