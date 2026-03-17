#pragma once

/**
 * @file qbuem/pipeline/health.hpp
 * @brief Pipeline health monitoring — PipelineHealth, ActionHealth, PipelineVersion
 * @defgroup qbuem_pipeline_health Pipeline Health
 * @ingroup qbuem_pipeline
 *
 * Tracks real-time health of pipelines and individual actions, and exports topology.
 *
 * ## Health hierarchy
 * ```
 * PipelineHealth          <- pipeline-level aggregate
 *   └─ ActionHealth[]     <- per-action detailed metrics
 * ```
 *
 * ## Versioning
 * Use `PipelineVersion` + `set_version()` / `compatible_with()` to manage schema changes safely.
 *
 * ## Topology export
 * `to_json()` — JSON, `to_dot()` — Graphviz DOT, `to_mermaid()` — Mermaid diagram.
 * @{
 */

#include <qbuem/pipeline/observability.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <functional>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace qbuem {

// ---------------------------------------------------------------------------
// PipelineVersion
// ---------------------------------------------------------------------------

/**
 * @brief Semantic versioning (major.minor.patch).
 *
 * `compatible_with(other)` treats matching major versions as compatible.
 */
struct PipelineVersion {
  uint32_t major{0};
  uint32_t minor{0};
  uint32_t patch{0};

  /// @brief Return version string (e.g. "1.2.3").
  [[nodiscard]] std::string to_string() const {
    return std::format("{}.{}.{}", major, minor, patch);
  }

  /// @brief Compatible if the major version matches.
  [[nodiscard]] bool compatible_with(const PipelineVersion &other) const noexcept {
    return major == other.major;
  }

  auto operator<=>(const PipelineVersion &) const = default;
};

// ---------------------------------------------------------------------------
// ActionHealth
// ---------------------------------------------------------------------------

/**
 * @brief Real-time health metrics for a single action.
 */
struct ActionHealth {
  std::string name;

  // Circuit breaker state: "CLOSED" / "OPEN" / "HALF_OPEN"
  std::string circuit_state{"CLOSED"};

  // Error rate over the last 1 minute (0.0 – 1.0)
  double error_rate_1m{0.0};

  // p99 latency (µs)
  uint64_t p99_us{0};

  // Number of items currently queued
  size_t queue_depth{0};

  // Total items processed
  uint64_t items_processed{0};

  /// @brief Serialize to a JSON string.
  [[nodiscard]] std::string to_json() const {
    return std::format(
        "{{\"name\":\"{}\",\"circuit_state\":\"{}\","
        "\"error_rate_1m\":{},\"p99_us\":{},\"queue_depth\":{},"
        "\"items_processed\":{}}}",
        name, circuit_state, error_rate_1m, p99_us, queue_depth, items_processed);
  }
};

// ---------------------------------------------------------------------------
// PipelineHealth
// ---------------------------------------------------------------------------

/**
 * @brief Overall pipeline health status.
 */
enum class HealthStatus { HEALTHY, DEGRADED, UNHEALTHY };

inline std::string_view to_string(HealthStatus s) noexcept {
  switch (s) {
    case HealthStatus::HEALTHY:   return "HEALTHY";
    case HealthStatus::DEGRADED:  return "DEGRADED";
    case HealthStatus::UNHEALTHY: return "UNHEALTHY";
  }
  return "UNKNOWN";
}

/**
 * @brief Pipeline-level health aggregate.
 */
struct PipelineHealth {
  std::string    name;
  HealthStatus   status{HealthStatus::HEALTHY};
  std::vector<ActionHealth> actions;

  // Recompute pipeline status from action states.
  void recompute() {
    size_t open_count = 0;
    for (const auto &a : actions) {
      if (a.circuit_state == "OPEN")   ++open_count;
    }
    if (open_count == 0)
      status = HealthStatus::HEALTHY;
    else if (open_count < actions.size())
      status = HealthStatus::DEGRADED;
    else
      status = HealthStatus::UNHEALTHY;
  }

  /// @brief Serialize to a JSON string.
  [[nodiscard]] std::string to_json() const {
    std::string actions_json;
    for (size_t i = 0; i < actions.size(); ++i) {
      if (i) actions_json += ',';
      actions_json += actions[i].to_json();
    }
    return std::format("{{\"name\":\"{}\",\"status\":\"{}\",\"actions\":[{}]}}",
                       name, to_string(status), actions_json);
  }
};

// ---------------------------------------------------------------------------
// HealthRegistry
// ---------------------------------------------------------------------------

/**
 * @brief Registry of pipeline health states.
 *
 * Manages a pipeline-name → PipelineHealth mapping.
 * Thread-safe.
 */
class HealthRegistry {
public:
  /// @brief Global singleton instance.
  static HealthRegistry &global() {
    static HealthRegistry instance;
    return instance;
  }

  /// @brief Register or update a pipeline health record.
  void update(PipelineHealth health) {
    std::unique_lock lock(mtx_);
    map_[health.name] = std::move(health);
  }

  /// @brief Look up a pipeline health record.
  [[nodiscard]] std::optional<PipelineHealth> get(std::string_view name) const {
    std::shared_lock lock(mtx_);
    auto it = map_.find(std::string(name));
    if (it == map_.end()) return std::nullopt;
    return it->second;
  }

  /// @brief Serialize all registered pipelines as a JSON array.
  [[nodiscard]] std::string all_json() const {
    std::shared_lock lock(mtx_);
    std::string result = "[";
    bool first = true;
    for (const auto &[_, h] : map_) {
      if (!first) result += ',';
      result += h.to_json();
      first = false;
    }
    result += ']';
    return result;
  }

private:
  mutable std::shared_mutex mtx_;
  std::unordered_map<std::string, PipelineHealth> map_;
};

// ---------------------------------------------------------------------------
// PipelineVersionRegistry
// ---------------------------------------------------------------------------

/**
 * @brief Pipeline version registry.
 *
 * Register versions with `set_version(pipeline_name, version)` and check
 * compatibility with `compatible_with(name, version)`.
 */
class PipelineVersionRegistry {
public:
  static PipelineVersionRegistry &global() {
    static PipelineVersionRegistry instance;
    return instance;
  }

  void set_version(std::string_view name, PipelineVersion ver) {
    std::unique_lock lock(mtx_);
    versions_[std::string(name)] = ver;
  }

  [[nodiscard]] std::optional<PipelineVersion> get(std::string_view name) const {
    std::shared_lock lock(mtx_);
    auto it = versions_.find(std::string(name));
    if (it == versions_.end()) return std::nullopt;
    return it->second;
  }

  [[nodiscard]] bool compatible_with(std::string_view name,
                                     const PipelineVersion &required) const {
    auto ver = get(name);
    return ver && ver->compatible_with(required);
  }

private:
  mutable std::shared_mutex mtx_;
  std::unordered_map<std::string, PipelineVersion> versions_;
};

// ---------------------------------------------------------------------------
// GraphTopologyExporter — to_json() for PipelineGraph
// ---------------------------------------------------------------------------

/**
 * @brief Exports pipeline graph topology in various formats.
 *
 * PipelineGraph already provides to_dot() / to_mermaid(); this helper adds JSON export.
 *
 * ## Usage
 * ```cpp
 * PipelineGraph<int> graph;
 * // ... add nodes and edges ...
 * std::string json = GraphTopologyExporter::to_json(graph);
 * ```
 */
struct GraphTopologyExporter {
  /**
   * @brief Serialize a PipelineGraph topology to JSON.
   *
   * Uses the graph's to_dot() output to extract nodes and edges.
   *
   * @tparam Graph  PipelineGraph<T> type.
   * @param  graph  Graph to serialize.
   * @returns JSON string.
   */
  template <typename Graph>
  static std::string to_json(const Graph &graph) {
    // Parse DOT output to extract nodes and edges.
    std::string dot = graph.to_dot();
    return dot_to_json(dot);
  }

private:
  // Extract nodes/edges from a DOT string and convert to JSON.
  static std::string dot_to_json(const std::string &dot) {
    std::vector<std::string> nodes;
    std::vector<std::pair<std::string, std::string>> edges;

    std::istringstream ss(dot);
    std::string line;
    while (std::getline(ss, line)) {
      // Edge line: "  A -> B;"
      auto arrow = line.find("->");
      if (arrow != std::string::npos) {
        auto from_raw = line.substr(0, arrow);
        auto to_raw   = line.substr(arrow + 2);
        auto trim = [](std::string s) -> std::string {
          size_t a = s.find_first_not_of(" \t\"[];");
          size_t b = s.find_last_not_of(" \t\"[];");
          return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
        };
        edges.push_back({trim(from_raw), trim(to_raw)});
        // Auto-collect nodes from edges.
        for (auto &n : {edges.back().first, edges.back().second}) {
          if (std::find(nodes.begin(), nodes.end(), n) == nodes.end())
            nodes.push_back(n);
        }
      }
    }

    std::string nodes_json;
    for (size_t i = 0; i < nodes.size(); ++i) {
      if (i) nodes_json += ',';
      nodes_json += std::format("\"{}\"", nodes[i]);
    }
    std::string edges_json;
    for (size_t i = 0; i < edges.size(); ++i) {
      if (i) edges_json += ',';
      edges_json += std::format("[\"{}\",\"{}\"]", edges[i].first, edges[i].second);
    }
    return std::format("{{\"nodes\":[{}],\"edges\":[{}]}}", nodes_json, edges_json);
  }
};

/** @} */

} // namespace qbuem
