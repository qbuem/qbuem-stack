#pragma once

/**
 * @file qbuem/pipeline/pipeline_graph.hpp
 * @brief DAG-based pipeline graph — PipelineGraph<T>
 * @defgroup qbuem_pipeline_graph PipelineGraph
 * @ingroup qbuem_pipeline
 *
 * PipelineGraph constructs a pipeline as a directed acyclic graph (DAG).
 * Supports fan-out (1→N), fan-in (N→1), and A/B routing.
 *
 * ## Usage example
 * ```cpp
 * PipelineGraph<int> graph;
 * graph.node("source", [](int x, ActionEnv) -> Task<Result<int>> { co_return x*2; })
 *      .node("sink",   [](int x, ActionEnv) -> Task<Result<int>> { co_return x+1; })
 *      .edge("source", "sink")
 *      .source("source")
 *      .sink("sink");
 * graph.start(dispatcher);
 * ```
 * @{
 */

#include <qbuem/pipeline/action.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/async_channel.hpp>
#include <qbuem/pipeline/concepts.hpp>
#include <qbuem/pipeline/context.hpp>
#include <qbuem/pipeline/service_registry.hpp>

#include <any>
#include <atomic>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <queue>
#include <shared_mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace qbuem {

/**
 * @brief Node metadata for a pipeline graph.
 */
struct GraphNode {
    std::string name;          ///< Node name (must be unique)
    size_t      workers   = 1; ///< Number of workers
    size_t      chan_cap  = 256; ///< Channel capacity
    bool        enabled   = true; ///< Whether the node is enabled
};

/**
 * @brief An edge in a pipeline graph.
 */
struct GraphEdge {
    std::string from; ///< Source node name
    std::string to;   ///< Destination node name
    /// @brief Optional routing predicate (nullptr = always route)
    std::function<bool(const std::any& item)> predicate;
};

/**
 * @brief DAG-based pipeline graph.
 *
 * Supports fan-out (1→N), fan-in (N→1), and A/B routing.
 *
 * @tparam T Message type flowing through the graph.
 */
template <typename T>
class PipelineGraph {
public:
    /**
     * @brief Construct a PipelineGraph.
     * @param default_chan_cap Default channel capacity.
     * @param registry Service registry (nullptr uses the global registry).
     */
    explicit PipelineGraph(size_t default_chan_cap = 256,
                           ServiceRegistry* registry = nullptr)
        : default_chan_cap_(default_chan_cap)
        , registry_(registry)
        , output_channel_(std::make_shared<AsyncChannel<ContextualItem<T>>>(default_chan_cap))
    {}

    PipelineGraph(const PipelineGraph&) = delete;
    PipelineGraph& operator=(const PipelineGraph&) = delete;
    PipelineGraph(PipelineGraph&&) = default;
    PipelineGraph& operator=(PipelineGraph&&) = default;

    // -------------------------------------------------------------------------
    // Graph construction
    // -------------------------------------------------------------------------

    /**
     * @brief Add a processing node.
     *
     * @tparam FnT Function type satisfying ActionFn<FnT, T, T>.
     * @param name     Node name (must be unique).
     * @param fn       Processing function: T -> Task<Result<T>>.
     * @param workers  Number of workers.
     * @param chan_cap Channel capacity (0 uses default_chan_cap).
     * @returns *this (for method chaining).
     */
    template <typename FnT>
    PipelineGraph& node(std::string name, FnT fn,
                        size_t workers = 1, size_t chan_cap = 0) {
        static_assert(ActionFn<FnT, T, T>,
                      "fn must satisfy ActionFn<FnT, T, T>");

        size_t cap = (chan_cap == 0) ? default_chan_cap_ : chan_cap;

        auto impl = std::make_shared<NodeImpl>();
        impl->meta.name     = name;
        impl->meta.workers  = workers;
        impl->meta.chan_cap = cap;
        impl->meta.enabled  = true;
        impl->fn         = to_full_action_fn<FnT, T, T>(std::move(fn));
        impl->in_channel  = std::make_shared<AsyncChannel<ContextualItem<T>>>(cap);
        impl->out_channel = std::make_shared<AsyncChannel<ContextualItem<T>>>(cap);
        impl->stop_src    = std::make_unique<std::stop_source>();

        nodes_[name] = std::move(impl);
        return *this;
    }

    /**
     * @brief Add a from → to edge (fan-out supported).
     *
     * @param from Source node name.
     * @param to   Destination node name.
     * @returns *this.
     */
    PipelineGraph& edge(std::string from, std::string to) {
        auto it = nodes_.find(from);
        if (it != nodes_.end()) {
            it->second->successors.push_back(to);
            it->second->predicates.push_back(nullptr); // always route
        }
        auto to_it = nodes_.find(to);
        if (to_it != nodes_.end()) {
            to_it->second->predecessors.push_back(from);
        }
        return *this;
    }

    /**
     * @brief Add a conditional edge (A/B routing).
     *
     * @param from      Source node name.
     * @param to        Destination node name.
     * @param predicate Routing predicate function.
     * @returns *this.
     */
    PipelineGraph& edge_if(std::string from, std::string to,
                           std::function<bool(const T&)> predicate) {
        auto it = nodes_.find(from);
        if (it != nodes_.end()) {
            it->second->successors.push_back(to);
            // Wrap the T-predicate in a std::any-predicate for storage
            it->second->predicates.push_back(
                [pred = std::move(predicate)](const std::any& a) -> bool {
                    if (const T* p = std::any_cast<T>(&a))
                        return pred(*p);
                    return false;
                });
        }
        auto to_it = nodes_.find(to);
        if (to_it != nodes_.end()) {
            to_it->second->predecessors.push_back(from);
        }
        return *this;
    }

    /**
     * @brief Mark a node as an entry point.
     *
     * @param node_name Source node name.
     * @returns *this.
     */
    PipelineGraph& source(std::string node_name) {
        auto it = nodes_.find(node_name);
        if (it != nodes_.end())
            it->second->is_source = true;
        if (std::find(sources_.begin(), sources_.end(), node_name) == sources_.end())
            sources_.push_back(std::move(node_name));
        return *this;
    }

    /**
     * @brief Mark a node as an exit point.
     *
     * @param node_name Sink node name.
     * @returns *this.
     */
    PipelineGraph& sink(std::string node_name) {
        auto it = nodes_.find(node_name);
        if (it != nodes_.end())
            it->second->is_sink = true;
        if (std::find(sinks_.begin(), sinks_.end(), node_name) == sinks_.end())
            sinks_.push_back(std::move(node_name));
        return *this;
    }

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    /**
     * @brief Topologically sort and start all nodes (Kahn's algorithm).
     *
     * @param dispatcher Dispatcher to run coroutines on.
     * @returns true if no cycle detected; false if a cycle is found.
     */
    bool start(Dispatcher& dispatcher) {
        auto order = topo_sort();
        if (!order)
            return false; // cycle detected

        for (const auto& name : *order) {
            auto it = nodes_.find(name);
            if (it == nodes_.end())
                continue;
            auto& n = it->second;

            // Spawn node workers
            for (size_t i = 0; i < n->meta.workers; ++i) {
                n->worker_count.fetch_add(1, std::memory_order_relaxed);
                dispatcher.spawn(node_worker(n, i));
            }

            // Spawn fan-out worker if node has successors
            if (!n->successors.empty()) {
                dispatcher.spawn(fanout_worker(n));
            }
        }

        // Sink nodes: their out_channel feeds the merged output_channel_
        for (const auto& sink_name : sinks_) {
            auto it = nodes_.find(sink_name);
            if (it == nodes_.end())
                continue;
            auto sink_out = it->second->out_channel;
            dispatcher.spawn(merge_to_output(sink_out, output_channel_));
        }

        return true;
    }

    /**
     * @brief Send an item to all source nodes (with backpressure).
     *
     * @param item Item to process.
     * @param ctx  Item context.
     * @returns Result<void>{} or an error.
     */
    Task<Result<void>> push(T item, Context ctx = {}) {
        if (sources_.empty())
            co_return unexpected(std::make_error_code(std::errc::no_such_process));
        for (const auto& src_name : sources_) {
            auto it = nodes_.find(src_name);
            if (it == nodes_.end())
                continue;
            auto result = co_await it->second->in_channel->send(
                ContextualItem<T>{item, ctx});
            if (!result)
                co_return result;
        }
        co_return Result<void>{};
    }

    /**
     * @brief Non-blocking push.
     *
     * @returns true if successfully sent to all sources.
     */
    bool try_push(T item, Context ctx = {}) {
        if (sources_.empty())
            return false;
        bool all_ok = true;
        for (const auto& src_name : sources_) {
            auto it = nodes_.find(src_name);
            if (it == nodes_.end())
                continue;
            if (!it->second->in_channel->try_send(ContextualItem<T>{item, ctx}))
                all_ok = false;
        }
        return all_ok;
    }

    /**
     * @brief Close all sources and wait for all nodes to finish processing.
     */
    Task<void> drain() {
        // Close all source in_channels
        for (const auto& src_name : sources_) {
            auto it = nodes_.find(src_name);
            if (it != nodes_.end())
                it->second->in_channel->close();
        }
        // Wait for all nodes to finish
        for (auto& [name, n] : nodes_) {
            while (n->worker_count.load(std::memory_order_acquire) > 0) {
                co_await yield_once();
            }
        }
        output_channel_->close();
        co_return;
    }

    /**
     * @brief Stop immediately.
     */
    void stop() {
        for (auto& [name, n] : nodes_) {
            if (n->stop_src)
                n->stop_src->request_stop();
            n->in_channel->close();
        }
        output_channel_->close();
    }

    /**
     * @brief Return the merged output channel from all sink nodes.
     */
    [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<T>>> output() const {
        return output_channel_;
    }

    // -------------------------------------------------------------------------
    // Topology export
    // -------------------------------------------------------------------------

    /**
     * @brief Export topology in Graphviz DOT format.
     */
    [[nodiscard]] std::string to_dot() const {
        std::string out = "digraph PipelineGraph {\n  rankdir=LR;\n";
        for (const auto& [name, n] : nodes_) {
            std::string_view shape = n->is_source ? "ellipse"
                                   : n->is_sink   ? "doublecircle"
                                                  : "box";
            out += std::format("  \"{}\" [shape={}];\n", name, shape);
            for (size_t i = 0; i < n->successors.size(); ++i) {
                if (n->predicates[i] != nullptr)
                    out += std::format("  \"{}\" -> \"{}\" [style=dashed, label=\"cond\"];\n",
                                       name, n->successors[i]);
                else
                    out += std::format("  \"{}\" -> \"{}\";\n", name, n->successors[i]);
            }
        }
        out += "}\n";
        return out;
    }

    /**
     * @brief Export topology in Mermaid diagram format.
     */
    [[nodiscard]] std::string to_mermaid() const {
        std::string out = "graph LR\n";
        for (const auto& [name, n] : nodes_) {
            for (size_t i = 0; i < n->successors.size(); ++i) {
                if (n->predicates[i] != nullptr)
                    out += std::format("  {} -->|cond| {}\n", name, n->successors[i]);
                else
                    out += std::format("  {} --> {}\n", name, n->successors[i]);
            }
        }
        return out;
    }

    // -------------------------------------------------------------------------
    // Runtime validation
    // -------------------------------------------------------------------------

    /**
     * @brief Validate the graph.
     *
     * - Verify all edge endpoints exist
     * - Check for disconnected nodes
     * - Confirm source/sink nodes exist
     *
     * @returns true if valid.
     */
    [[nodiscard]] bool validate() const {
        if (nodes_.empty())
            return false;
        if (sources_.empty() || sinks_.empty())
            return false;

        // All sources must exist
        for (const auto& src : sources_) {
            if (nodes_.find(src) == nodes_.end())
                return false;
        }
        // All sinks must exist
        for (const auto& snk : sinks_) {
            if (nodes_.find(snk) == nodes_.end())
                return false;
        }
        // All successor targets must exist
        for (const auto& [name, n] : nodes_) {
            for (const auto& succ : n->successors) {
                if (nodes_.find(succ) == nodes_.end())
                    return false;
            }
        }
        return true;
    }

private:
    // -------------------------------------------------------------------------
    // Internal node structure
    // -------------------------------------------------------------------------
    struct NodeImpl {
        GraphNode   meta;
        std::function<Task<Result<T>>(T, ActionEnv)> fn;
        std::shared_ptr<AsyncChannel<ContextualItem<T>>> in_channel;
        std::shared_ptr<AsyncChannel<ContextualItem<T>>> out_channel;
        std::vector<std::string>                         successors;
        std::vector<std::function<bool(const std::any&)>> predicates; ///< per successor; nullptr = always
        std::vector<std::string>                         predecessors;
        std::unique_ptr<std::stop_source>                stop_src;
        std::atomic<size_t>                              worker_count{0};
        bool is_source = false;
        bool is_sink   = false;
    };

    // -------------------------------------------------------------------------
    // Kahn's topological sort
    // -------------------------------------------------------------------------

    /**
     * @brief Perform topological sort using Kahn's algorithm.
     *
     * @returns Sorted list of node names; std::nullopt if a cycle is detected.
     */
    [[nodiscard]] std::optional<std::vector<std::string>> topo_sort() const {
        // Compute in-degree for each node
        std::unordered_map<std::string, int> in_degree;
        for (const auto& [name, n] : nodes_)
            in_degree[name] = 0;
        for (const auto& [name, n] : nodes_) {
            for (const auto& succ : n->successors) {
                if (in_degree.find(succ) != in_degree.end())
                    in_degree[succ]++;
            }
        }

        std::queue<std::string> q;
        for (const auto& [name, deg] : in_degree) {
            if (deg == 0)
                q.push(name);
        }

        std::vector<std::string> order;
        order.reserve(nodes_.size());

        while (!q.empty()) {
            auto name = q.front();
            q.pop();
            order.push_back(name);
            auto it = nodes_.find(name);
            if (it == nodes_.end())
                continue;
            for (const auto& succ : it->second->successors) {
                auto& deg = in_degree[succ];
                if (--deg == 0)
                    q.push(succ);
            }
        }

        if (order.size() != nodes_.size())
            return std::nullopt; // cycle detected

        return order;
    }

    // -------------------------------------------------------------------------
    // Worker coroutines
    // -------------------------------------------------------------------------

    /**
     * @brief Single node worker coroutine.
     *
     * Reads items from the input channel, applies fn, and forwards to the output channel.
     */
    Task<void> node_worker(std::shared_ptr<NodeImpl> n, size_t worker_idx) {
        auto stop_token = n->stop_src ? n->stop_src->get_token() : std::stop_token{};

        for (;;) {
            auto citem = co_await n->in_channel->recv();
            if (!citem) break; // EOS

            ActionEnv env{
                .ctx        = citem->ctx,
                .stop       = stop_token,
                .worker_idx = worker_idx,
                .registry   = registry_ ? registry_ : &global_registry(),
            };

            auto result = co_await n->fn(std::move(citem->value), env);

            if (result.has_value() && n->out_channel) {
                co_await n->out_channel->send(
                    ContextualItem<T>{std::move(*result), env.ctx});
            }
        }

        size_t remaining = n->worker_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0 && n->out_channel)
            n->out_channel->close();
        co_return;
    }

    /**
     * @brief Fan-out worker coroutine.
     *
     * Reads from the node's out_channel and distributes to all successor in_channels.
     * Evaluates conditional predicates before routing when present.
     */
    Task<void> fanout_worker(std::shared_ptr<NodeImpl> n) {
        for (;;) {
            auto citem = co_await n->out_channel->recv();
            if (!citem) {
                // Close all successor in_channels
                for (const auto& succ_name : n->successors) {
                    auto it = nodes_.find(succ_name);
                    if (it != nodes_.end())
                        it->second->in_channel->close();
                }
                co_return;
            }

            std::any item_any = citem->value; // wrap for predicate evaluation
            for (size_t i = 0; i < n->successors.size(); ++i) {
                // Check predicate
                if (n->predicates[i] != nullptr && !n->predicates[i](item_any))
                    continue;

                auto it = nodes_.find(n->successors[i]);
                if (it == nodes_.end())
                    continue;
                // Fan-out: copy item for each successor
                co_await it->second->in_channel->send(
                    ContextualItem<T>{citem->value, citem->ctx});
            }
        }
    }

    /**
     * @brief Merge a sink node's output channel into the unified output channel.
     */
    Task<void> merge_to_output(
        std::shared_ptr<AsyncChannel<ContextualItem<T>>> src,
        std::shared_ptr<AsyncChannel<ContextualItem<T>>> dst)
    {
        for (;;) {
            auto citem = co_await src->recv();
            if (!citem) co_return;
            co_await dst->send(std::move(*citem));
        }
    }

    // -------------------------------------------------------------------------
    // Coroutine yield helper
    // -------------------------------------------------------------------------
    struct YieldAwaiter {
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) noexcept {
            if (auto* r = Reactor::current())
                r->post([h]() mutable { h.resume(); });
            else
                h.resume();
        }
        void await_resume() noexcept {}
    };

    static Task<void> yield_once() {
        co_await YieldAwaiter{};
        co_return;
    }

    // -------------------------------------------------------------------------
    // Data members
    // -------------------------------------------------------------------------
    std::unordered_map<std::string, std::shared_ptr<NodeImpl>> nodes_;
    std::vector<std::string>                                   sources_;
    std::vector<std::string>                                   sinks_;
    std::shared_ptr<AsyncChannel<ContextualItem<T>>>           output_channel_;
    size_t                                                     default_chan_cap_;
    ServiceRegistry*                                           registry_;
};

} // namespace qbuem

/** @} */
