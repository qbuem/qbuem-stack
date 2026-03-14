#pragma once

/**
 * @file qbuem/pipeline/pipeline_factory.hpp
 * @brief Config-driven 파이프라인 팩토리 — PipelineFactory
 * @defgroup qbuem_pipeline_factory PipelineFactory
 * @ingroup qbuem_pipeline
 *
 * JSON 설정으로 DynamicPipeline / PipelineGraph 를 생성합니다.
 * 플러그인 등록(register_plugin)을 통해 이름 → 액션 함수를 매핑하고,
 * from_json() / graph_from_json()으로 런타임에 파이프라인을 조립합니다.
 *
 * ## JSON 스키마
 *
 * ### DynamicPipeline (선형)
 * ```json
 * {
 *   "type": "linear",
 *   "stages": [
 *     { "name": "double", "plugin": "multiplier", "config": { "factor": 2 } },
 *     { "name": "log",    "plugin": "logger" }
 *   ]
 * }
 * ```
 *
 * ### PipelineGraph (DAG)
 * ```json
 * {
 *   "type": "graph",
 *   "nodes": [
 *     { "name": "A", "plugin": "validator" },
 *     { "name": "B", "plugin": "enricher" },
 *     { "name": "C", "plugin": "sink" }
 *   ],
 *   "edges": [["A","B"], ["A","C"]],
 *   "source": "A",
 *   "sink":   "C"
 * }
 * ```
 *
 * ## 사용 예시
 * ```cpp
 * PipelineFactory<int> factory;
 * factory.register_plugin("multiplier", [](const nlohmann::json& cfg) {
 *   int factor = cfg.value("factor", 1);
 *   return [factor](int x, ActionEnv) -> Task<Result<int>> { co_return x * factor; };
 * });
 *
 * auto dp = factory.from_json(R"({"type":"linear","stages":[
 *   {"name":"x2","plugin":"multiplier","config":{"factor":2}}
 * ]})");
 * dp->start(dispatcher);
 * ```
 * @{
 */

#include <qbuem/pipeline/dynamic_pipeline.hpp>
#include <qbuem/pipeline/pipeline_graph.hpp>

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

// Optional: use nlohmann/json if available; otherwise provide a minimal shim.
#if __has_include(<nlohmann/json.hpp>)
#  include <nlohmann/json.hpp>
   namespace qbuem::detail { using Json = nlohmann::json; }
#elif __has_include(<qbuem_json/qbuem_json.hpp>)
#  include <qbuem_json/qbuem_json.hpp>
   // qbuem_json does not expose an object type; use a thin wrapper.
   namespace qbuem::detail {
     // Minimal JSON value wrapping a string for the factory's needs.
     struct Json {
       std::string raw;
       Json() = default;
       explicit Json(std::string_view s) : raw(s) {}
       // value(key, default) always returns the default — sufficient for
       // factories that ignore config when the JSON library is unavailable.
       template <typename T> T value(std::string_view, T def) const { return def; }
       bool contains(std::string_view) const { return false; }
     };
   }
#else
   namespace qbuem::detail {
     struct Json {
       template <typename T> T value(std::string_view, T def) const { return def; }
       bool contains(std::string_view) const { return false; }
     };
   }
#endif

namespace qbuem {

/**
 * @brief Config-driven 파이프라인 팩토리.
 *
 * @tparam T 파이프라인 메시지 타입 (DynamicPipeline<T> / PipelineGraph<T>).
 */
template <typename T>
class PipelineFactory {
public:
  /// @brief 플러그인 팩토리 함수 타입.
  /// JSON 설정을 받아 액션 함수를 반환합니다.
  using PluginFactory =
      std::function<typename DynamicPipeline<T>::StageFn(const detail::Json &)>;

  // -------------------------------------------------------------------------
  // 플러그인 등록
  // -------------------------------------------------------------------------

  /**
   * @brief 플러그인 이름과 팩토리 함수를 등록합니다.
   *
   * @param plugin_name JSON의 "plugin" 필드에서 사용할 이름.
   * @param factory     JSON config 객체를 받아 액션 함수를 반환하는 팩토리.
   * @throws std::invalid_argument 이름이 비어 있거나 이미 등록된 경우.
   */
  PipelineFactory &register_plugin(std::string plugin_name, PluginFactory factory) {
    if (plugin_name.empty())
      throw std::invalid_argument("plugin name must not be empty");
    if (plugins_.count(plugin_name))
      throw std::invalid_argument("plugin already registered: " + plugin_name);
    plugins_.emplace(std::move(plugin_name), std::move(factory));
    return *this;
  }

  /**
   * @brief 플러그인이 등록되어 있는지 확인합니다.
   *
   * @param plugin_name 조회할 플러그인 이름.
   * @returns 등록된 경우 true.
   */
  [[nodiscard]] bool has_plugin(std::string_view plugin_name) const {
    return plugins_.count(std::string(plugin_name)) > 0;
  }

  // -------------------------------------------------------------------------
  // 선형 파이프라인 생성
  // -------------------------------------------------------------------------

  /**
   * @brief JSON 문자열에서 DynamicPipeline<T>을 생성합니다.
   *
   * 스키마:
   * ```json
   * {
   *   "type": "linear",
   *   "stages": [
   *     { "name": "<stage-name>", "plugin": "<plugin-name>", "config": {} }
   *   ]
   * }
   * ```
   *
   * @param json_str JSON 설정 문자열.
   * @returns 구성된 DynamicPipeline (시작 전 상태).
   * @throws std::runtime_error 파싱 실패, 알 수 없는 플러그인, 스키마 오류.
   */
  [[nodiscard]] std::unique_ptr<DynamicPipeline<T>> from_json(std::string_view json_str) {
    auto doc = parse_json(json_str);
    return build_linear(doc);
  }

  /**
   * @brief JSON 문자열에서 PipelineGraph<T>을 생성합니다.
   *
   * 스키마:
   * ```json
   * {
   *   "type": "graph",
   *   "nodes":  [ { "name": "A", "plugin": "p", "config": {} } ],
   *   "edges":  [["A","B"], ["A","C"]],
   *   "source": "A",
   *   "sink":   "C"
   * }
   * ```
   *
   * @param json_str JSON 설정 문자열.
   * @returns 구성된 PipelineGraph (시작 전 상태).
   * @throws std::runtime_error 파싱 실패, 알 수 없는 플러그인, 스키마 오류.
   */
  [[nodiscard]] std::unique_ptr<PipelineGraph<T>> graph_from_json(std::string_view json_str) {
    auto doc = parse_json(json_str);
    return build_graph(doc);
  }

private:
  std::unordered_map<std::string, PluginFactory> plugins_;

  // -------------------------------------------------------------------------
  // Internal helpers
  // -------------------------------------------------------------------------

  static detail::Json parse_json(std::string_view s) {
#if __has_include(<nlohmann/json.hpp>)
    try {
      return nlohmann::json::parse(s);
    } catch (const nlohmann::json::exception &e) {
      throw std::runtime_error(std::string("PipelineFactory: JSON parse error: ") + e.what());
    }
#else
    // No JSON library: return a dummy object. Users can call from_json() only
    // if nlohmann/json is available; otherwise they must use the programmatic API.
    (void)s;
    throw std::runtime_error(
        "PipelineFactory: JSON parsing requires nlohmann/json. "
        "Add it to your CMakeLists.txt or use the programmatic API.");
#endif
  }

  typename DynamicPipeline<T>::StageFn make_stage_fn(const detail::Json &stage_cfg) {
#if __has_include(<nlohmann/json.hpp>)
    if (!stage_cfg.contains("plugin"))
      throw std::runtime_error("PipelineFactory: stage missing 'plugin' field");

    std::string plugin_name = stage_cfg["plugin"].template get<std::string>();
    auto it = plugins_.find(plugin_name);
    if (it == plugins_.end())
      throw std::runtime_error("PipelineFactory: unknown plugin '" + plugin_name + "'");

    detail::Json config;
    if (stage_cfg.contains("config"))
      config = stage_cfg["config"];

    return it->second(config);
#else
    (void)stage_cfg;
    throw std::runtime_error("PipelineFactory: requires nlohmann/json");
#endif
  }

  std::unique_ptr<DynamicPipeline<T>> build_linear(const detail::Json &doc) {
#if __has_include(<nlohmann/json.hpp>)
    auto pipeline = std::make_unique<DynamicPipeline<T>>();

    if (!doc.contains("stages"))
      throw std::runtime_error("PipelineFactory: 'stages' array missing in JSON");

    for (const auto &stage : doc["stages"]) {
      std::string name = stage.value("name", std::string{});
      if (name.empty())
        throw std::runtime_error("PipelineFactory: stage missing 'name' field");

      auto fn = make_stage_fn(stage);
      pipeline->add_stage(name, std::move(fn));
    }

    return pipeline;
#else
    (void)doc;
    throw std::runtime_error("PipelineFactory: requires nlohmann/json");
#endif
  }

  std::unique_ptr<PipelineGraph<T>> build_graph(const detail::Json &doc) {
#if __has_include(<nlohmann/json.hpp>)
    auto graph = std::make_unique<PipelineGraph<T>>();

    if (!doc.contains("nodes"))
      throw std::runtime_error("PipelineFactory: 'nodes' array missing in JSON");

    // Add nodes
    for (const auto &node : doc["nodes"]) {
      std::string name = node.value("name", std::string{});
      if (name.empty())
        throw std::runtime_error("PipelineFactory: node missing 'name' field");

      auto fn = make_stage_fn(node);
      graph->node(name, std::move(fn));
    }

    // Add edges
    if (doc.contains("edges")) {
      for (const auto &edge : doc["edges"]) {
        if (!edge.is_array() || edge.size() < 2)
          throw std::runtime_error("PipelineFactory: each edge must be [from, to]");
        graph->edge(edge[0].template get<std::string>(),
                    edge[1].template get<std::string>());
      }
    }

    // Source / sink
    if (doc.contains("source"))
      graph->source(doc["source"].template get<std::string>());
    if (doc.contains("sink"))
      graph->sink(doc["sink"].template get<std::string>());

    return graph;
#else
    (void)doc;
    throw std::runtime_error("PipelineFactory: requires nlohmann/json");
#endif
  }
};

/** @} */

} // namespace qbuem
