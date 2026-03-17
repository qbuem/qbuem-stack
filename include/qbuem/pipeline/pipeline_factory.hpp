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

// Optional: use nlohmann/json or qbuem_json if available; otherwise stub.
#if __has_include(<nlohmann/json.hpp>)
#  include <nlohmann/json.hpp>
   namespace qbuem::detail { using Json = nlohmann::json; }
#elif __has_include(<qbuem_json/qbuem_json.hpp>)
#  include <qbuem_json/qbuem_json.hpp>
#  include <cstddef>
#  include <memory>
   namespace qbuem::detail {

     // Forward declaration for the iterator.
     class Json;

     /// qbuem_json Value 기반 배열 이터레이터.
     class JsonIterator {
     public:
       JsonIterator() = default;
       JsonIterator(std::shared_ptr<qbuem::Document> d,
                    qbuem::Value arr, std::size_t idx)
           : doc_(std::move(d)), arr_(std::move(arr)), idx_(idx) {}

       Json operator*() const;          // defined after Json
       JsonIterator& operator++() { ++idx_; return *this; }
       bool operator!=(const JsonIterator& o) const { return idx_ != o.idx_; }

     private:
       std::shared_ptr<qbuem::Document> doc_;
       qbuem::Value arr_;
       std::size_t  idx_ = 0;
     };

     /**
      * @brief qbuem_json Value 래퍼 — nlohmann::json 호환 인터페이스 제공.
      *
      * Document 수명은 shared_ptr 으로 공유하므로 sub-value 반환이 안전합니다.
      */
     class Json {
     public:
       // ── 생성 ──────────────────────────────────────────────────────────────

       /// 빈 JSON 오브젝트 `{}` 로 기본 생성.
       Json() : doc_(std::make_shared<qbuem::Document>()),
                val_(qbuem::parse(*doc_, "{}")) {}

       /// JSON 문자열로부터 파싱.
       explicit Json(std::string_view s)
           : doc_(std::make_shared<qbuem::Document>()),
             val_(qbuem::parse(*doc_, s)) {}

       /// 부모 Document 를 공유하는 sub-value 생성 (내부 전용).
       Json(std::shared_ptr<qbuem::Document> d, qbuem::Value v)
           : doc_(std::move(d)), val_(std::move(v)) {}

       // ── 키 존재 확인 ─────────────────────────────────────────────────────

       bool contains(std::string_view key) const {
           return val_.contains(std::string(key));
       }

       // ── 값 접근 ──────────────────────────────────────────────────────────

       /// 키에 해당하는 값을 반환. 없으면 def 반환.
       template <typename T>
       T value(std::string_view key, T def) const {
           if (!contains(key)) return def;
           try { return val_[std::string(key)].template as<T>(); }
           catch (...) { return def; }
       }

       /// 이 Value 자체를 T 로 변환 (리프 값에 사용).
       template <typename T>
       T get() const {
           return val_.template as<T>();
       }

       // ── 서브-값 접근 ─────────────────────────────────────────────────────

       Json operator[](std::string_view key) const {
           return Json(doc_, val_[std::string(key)]);
       }

       Json operator[](std::size_t idx) const {
           return Json(doc_, val_[idx]);
       }

       // ── 타입 / 크기 ──────────────────────────────────────────────────────

       bool        is_array() const { return val_.is_array(); }
       std::size_t size()     const { return val_.size(); }

       // ── 배열 이터레이션 ───────────────────────────────────────────────────

       JsonIterator begin() const { return {doc_, val_, 0};          }
       JsonIterator end()   const { return {doc_, val_, val_.size()}; }

     // private: 내부 헬퍼가 접근할 수 있도록 공개 유지.
       std::shared_ptr<qbuem::Document> doc_;
       qbuem::Value                     val_;
     };

     // JsonIterator::operator* — Json 정의 후 구현.
     inline Json JsonIterator::operator*() const {
         return Json(doc_, arr_[idx_]);
     }

   } // namespace qbuem::detail
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
   * @brief Register a plugin name and factory function.
   *
   * @param plugin_name  Name used in the JSON "plugin" field.
   * @param factory      Factory that accepts a JSON config object and returns a stage function.
   * @throws std::invalid_argument if the name is empty or already registered.
   */
  PipelineFactory &register_plugin(std::string plugin_name, PluginFactory factory) {
    if (plugin_name.empty())
      throw std::invalid_argument("plugin name must not be empty");
    if (plugins_.contains(plugin_name))
      throw std::invalid_argument("plugin already registered: " + plugin_name);
    plugins_.emplace(std::move(plugin_name), std::move(factory));
    return *this;
  }

  /**
   * @brief Check whether a plugin is registered.
   *
   * @param plugin_name  Plugin name to look up.
   * @returns true if registered.
   */
  [[nodiscard]] bool has_plugin(std::string_view plugin_name) const {
    return plugins_.contains(std::string(plugin_name));
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
#elif __has_include(<qbuem_json/qbuem_json.hpp>)
    try {
      return detail::Json(s);
    } catch (...) {
      throw std::runtime_error("PipelineFactory: JSON parse error");
    }
#else
    (void)s;
    throw std::runtime_error(
        "PipelineFactory: JSON parsing requires nlohmann/json or qbuem_json. "
        "Add one to your CMakeLists.txt or use the programmatic API.");
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
#elif __has_include(<qbuem_json/qbuem_json.hpp>)
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
    throw std::runtime_error("PipelineFactory: requires nlohmann/json or qbuem_json");
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
#elif __has_include(<qbuem_json/qbuem_json.hpp>)
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
    throw std::runtime_error("PipelineFactory: requires nlohmann/json or qbuem_json");
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
#elif __has_include(<qbuem_json/qbuem_json.hpp>)
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
    throw std::runtime_error("PipelineFactory: requires nlohmann/json or qbuem_json");
#endif
  }
};

/** @} */

} // namespace qbuem
