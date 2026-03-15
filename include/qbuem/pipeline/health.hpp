#pragma once

/**
 * @file qbuem/pipeline/health.hpp
 * @brief 파이프라인 상태 모니터링 — PipelineHealth, ActionHealth, PipelineVersion
 * @defgroup qbuem_pipeline_health Pipeline Health
 * @ingroup qbuem_pipeline
 *
 * 파이프라인과 개별 액션의 실시간 상태를 추적하고 토폴로지를 내보냅니다.
 *
 * ## 상태 계층
 * ```
 * PipelineHealth          ← 파이프라인 단위 집계
 *   └─ ActionHealth[]     ← 액션별 세부 지표
 * ```
 *
 * ## 버저닝
 * `PipelineVersion` + `set_version()` / `compatible_with()` 로
 * 스키마 변경을 안전하게 관리합니다.
 *
 * ## 토폴로지 Export
 * `to_json()` — JSON, `to_dot()` — Graphviz DOT, `to_mermaid()` — Mermaid 다이어그램.
 * @{
 */

#include <qbuem/pipeline/observability.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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
 * @brief 시맨틱 버저닝 (major.minor.patch).
 *
 * `compatible_with(other)` 는 major가 같으면 호환으로 간주합니다.
 */
struct PipelineVersion {
  uint32_t major{0};
  uint32_t minor{0};
  uint32_t patch{0};

  /// @brief 버전 문자열 반환 (e.g. "1.2.3").
  [[nodiscard]] std::string to_string() const {
    return std::to_string(major) + '.' +
           std::to_string(minor) + '.' +
           std::to_string(patch);
  }

  /// @brief major 버전이 같으면 호환.
  [[nodiscard]] bool compatible_with(const PipelineVersion &other) const noexcept {
    return major == other.major;
  }

  auto operator<=>(const PipelineVersion &) const = default;
};

// ---------------------------------------------------------------------------
// ActionHealth
// ---------------------------------------------------------------------------

/**
 * @brief 액션 단위 실시간 건강 지표.
 */
struct ActionHealth {
  std::string name;

  // Circuit breaker 상태 (문자열; "CLOSED" / "OPEN" / "HALF_OPEN")
  std::string circuit_state{"CLOSED"};

  // 최근 1분 에러율 (0.0 ~ 1.0)
  double error_rate_1m{0.0};

  // p99 레이턴시 (µs)
  uint64_t p99_us{0};

  // 현재 대기 중인 아이템 수
  size_t queue_depth{0};

  // 처리한 총 아이템 수
  uint64_t items_processed{0};

  /// @brief JSON 문자열로 직렬화합니다.
  [[nodiscard]] std::string to_json() const {
    std::ostringstream ss;
    ss << "{"
       << "\"name\":\"" << name << "\","
       << "\"circuit_state\":\"" << circuit_state << "\","
       << "\"error_rate_1m\":" << error_rate_1m << ","
       << "\"p99_us\":" << p99_us << ","
       << "\"queue_depth\":" << queue_depth << ","
       << "\"items_processed\":" << items_processed
       << "}";
    return ss.str();
  }
};

// ---------------------------------------------------------------------------
// PipelineHealth
// ---------------------------------------------------------------------------

/**
 * @brief 파이프라인 전체 건강 상태.
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
 * @brief 파이프라인 단위 건강 집계.
 */
struct PipelineHealth {
  std::string    name;
  HealthStatus   status{HealthStatus::HEALTHY};
  std::vector<ActionHealth> actions;

  // 재계산: 액션 상태를 토대로 파이프라인 상태를 집계합니다.
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

  /// @brief JSON 문자열로 직렬화합니다.
  [[nodiscard]] std::string to_json() const {
    std::ostringstream ss;
    ss << "{\"name\":\"" << name << "\","
       << "\"status\":\"" << to_string(status) << "\","
       << "\"actions\":[";
    for (size_t i = 0; i < actions.size(); ++i) {
      if (i) ss << ',';
      ss << actions[i].to_json();
    }
    ss << "]}";
    return ss.str();
  }
};

// ---------------------------------------------------------------------------
// HealthRegistry
// ---------------------------------------------------------------------------

/**
 * @brief 파이프라인 건강 상태 레지스트리.
 *
 * 파이프라인 이름 → PipelineHealth 매핑을 관리합니다.
 * 스레드 안전합니다.
 */
class HealthRegistry {
public:
  /// @brief 글로벌 싱글턴 인스턴스.
  static HealthRegistry &global() {
    static HealthRegistry instance;
    return instance;
  }

  /// @brief 파이프라인 건강 상태를 등록/갱신합니다.
  void update(PipelineHealth health) {
    std::unique_lock lock(mtx_);
    map_[health.name] = std::move(health);
  }

  /// @brief 파이프라인 건강 상태를 조회합니다.
  [[nodiscard]] std::optional<PipelineHealth> get(std::string_view name) const {
    std::shared_lock lock(mtx_);
    auto it = map_.find(std::string(name));
    if (it == map_.end()) return std::nullopt;
    return it->second;
  }

  /// @brief 등록된 모든 파이프라인을 JSON 배열로 직렬화합니다.
  [[nodiscard]] std::string all_json() const {
    std::shared_lock lock(mtx_);
    std::ostringstream ss;
    ss << '[';
    bool first = true;
    for (const auto &[_, h] : map_) {
      if (!first) ss << ',';
      ss << h.to_json();
      first = false;
    }
    ss << ']';
    return ss.str();
  }

private:
  mutable std::shared_mutex mtx_;
  std::unordered_map<std::string, PipelineHealth> map_;
};

// ---------------------------------------------------------------------------
// PipelineVersionRegistry
// ---------------------------------------------------------------------------

/**
 * @brief 파이프라인 버전 레지스트리.
 *
 * `set_version(pipeline_name, version)` 으로 버전을 등록하고
 * `compatible_with(name, version)` 으로 호환성을 검사합니다.
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
 * @brief 파이프라인 그래프 토폴로지를 다양한 형식으로 내보냅니다.
 *
 * PipelineGraph는 이미 to_dot() / to_mermaid()을 가지고 있으므로,
 * 이 헬퍼는 JSON 형식을 추가합니다.
 *
 * ## 사용법
 * ```cpp
 * PipelineGraph<int> graph;
 * // ... 노드 및 엣지 추가 ...
 * std::string json = GraphTopologyExporter::to_json(graph);
 * ```
 */
struct GraphTopologyExporter {
  /**
   * @brief PipelineGraph 토폴로지를 JSON으로 직렬화합니다.
   *
   * 그래프의 `.node_names()`, `.edge_list()`, `.source_names()`, `.sink_names()`
   * 메서드를 사용합니다. 해당 메서드가 없으면 graph의 to_dot()을 파싱하여 추출합니다.
   *
   * @tparam Graph PipelineGraph<T> 타입.
   * @param  graph 직렬화할 그래프.
   * @returns JSON 문자열.
   */
  template <typename Graph>
  static std::string to_json(const Graph &graph) {
    // dot 출력을 파싱하여 노드와 엣지를 추출합니다.
    std::string dot = graph.to_dot();
    return dot_to_json(dot);
  }

private:
  // DOT 문자열에서 노드/엣지를 추출하여 JSON으로 변환합니다.
  static std::string dot_to_json(const std::string &dot) {
    std::vector<std::string> nodes;
    std::vector<std::pair<std::string, std::string>> edges;

    std::istringstream ss(dot);
    std::string line;
    while (std::getline(ss, line)) {
      // 엣지 라인: "  A -> B;"
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
        // 노드 자동 수집
        for (auto &n : {edges.back().first, edges.back().second}) {
          if (std::find(nodes.begin(), nodes.end(), n) == nodes.end())
            nodes.push_back(n);
        }
      }
    }

    std::ostringstream out;
    out << "{\"nodes\":[";
    for (size_t i = 0; i < nodes.size(); ++i) {
      if (i) out << ',';
      out << "\"" << nodes[i] << "\"";
    }
    out << "],\"edges\":[";
    for (size_t i = 0; i < edges.size(); ++i) {
      if (i) out << ',';
      out << "[\"" << edges[i].first << "\",\"" << edges[i].second << "\"]";
    }
    out << "]}";
    return out.str();
  }
};

/** @} */

} // namespace qbuem
