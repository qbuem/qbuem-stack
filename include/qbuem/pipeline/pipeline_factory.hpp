#pragma once

/**
 * @file qbuem/pipeline/pipeline_factory.hpp
 * @brief Config-driven pipeline factory — PipelineFactory
 * @defgroup qbuem_pipeline_factory PipelineFactory
 * @ingroup qbuem_pipeline
 *
 * Creates DynamicPipeline / PipelineGraph from JSON configuration.
 * Maps names to action functions via register_plugin(),
 * and assembles pipelines at runtime with from_json() / graph_from_json().
 *
 * ## JSON schema
 *
 * ### DynamicPipeline (linear)
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
 * ### PipelineGraph (DAG, graph)
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
 * ## Usage example
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

     /// Array iterator based on qbuem_json Value.
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
      * @brief qbuem_json Value wrapper — provides an nlohmann::json-compatible interface.
      *
      * Document lifetime is shared via shared_ptr, so returning sub-values is safe.
      */
     class Json {
     public:
       // ── Construction ──────────────────────────────────────────────────────

       /// Default-constructs as an empty JSON object `{}`.
       Json() : doc_(std::make_shared<qbuem::Document>()),
                val_(qbuem::parse(*doc_, "{}")) {}

       /// Parses from a JSON string.
       explicit Json(std::string_view s)
           : doc_(std::make_shared<qbuem::Document>()),
             val_(qbuem::parse(*doc_, s)) {}

       /// Constructs a sub-value sharing the parent Document (internal use only).
       Json(std::shared_ptr<qbuem::Document> d, qbuem::Value v)
           : doc_(std::move(d)), val_(std::move(v)) {}

       // ── Key existence check ───────────────────────────────────────────────

       bool contains(std::string_view key) const {
           return val_.contains(std::string(key));
       }

       // ── Value access ─────────────────────────────────────────────────────

       /// Returns the value for the given key, or def if not found.
       template <typename T>
       T value(std::string_view key, T def) const {
           if (!contains(key)) return def;
           try { return val_[std::string(key)].template as<T>(); }
           catch (...) { return def; }
       }

       /// Converts this Value itself to T (used for leaf values).
       template <typename T>
       T get() const {
           return val_.template as<T>();
       }

       // ── Sub-value access ─────────────────────────────────────────────────

       Json operator[](std::string_view key) const {
           return Json(doc_, val_[std::string(key)]);
       }

       Json operator[](std::size_t idx) const {
           return Json(doc_, val_[idx]);
       }

       // ── Type / size ──────────────────────────────────────────────────────

       bool        is_array() const { return val_.is_array(); }
       std::size_t size()     const { return val_.size(); }

       // ── Array iteration ──────────────────────────────────────────────────

       JsonIterator begin() const { return {doc_, val_, 0};          }
       JsonIterator end()   const { return {doc_, val_, val_.size()}; }

     // private: kept public so internal helpers can access it.
       std::shared_ptr<qbuem::Document> doc_;
       qbuem::Value                     val_;
     };

     // JsonIterator::operator* — implemented after Json is defined.
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
 * @brief Config-driven pipeline factory.
 *
 * @tparam T Pipeline message type (DynamicPipeline<T> / PipelineGraph<T>).
 */
template <typename T>
class PipelineFactory {
public:
  /// @brief Plugin factory function type.
  /// Accepts a JSON config and returns an action function.
  using PluginFactory =
      std::function<typename DynamicPipeline<T>::StageFn(const detail::Json &)>;

  // -------------------------------------------------------------------------
  // Plugin registration
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
  // Linear pipeline construction
  // -------------------------------------------------------------------------

  /**
   * @brief Constructs a DynamicPipeline<T> from a JSON string.
   *
   * Schema:
   * ```json
   * {
   *   "type": "linear",
   *   "stages": [
   *     { "name": "<stage-name>", "plugin": "<plugin-name>", "config": {} }
   *   ]
   * }
   * ```
   *
   * @param json_str JSON configuration string.
   * @returns Configured DynamicPipeline (not yet started).
   * @throws std::runtime_error on parse failure, unknown plugin, or schema error.
   */
  [[nodiscard]] std::unique_ptr<DynamicPipeline<T>> from_json(std::string_view json_str) {
    auto doc = parse_json(json_str);
    return build_linear(doc);
  }

  /**
   * @brief Constructs a PipelineGraph<T> from a JSON string.
   *
   * Schema:
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
   * @param json_str JSON configuration string.
   * @returns Configured PipelineGraph (not yet started).
   * @throws std::runtime_error on parse failure, unknown plugin, or schema error.
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
