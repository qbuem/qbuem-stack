#pragma once

/**
 * @file qbuem/pipeline/pipeline_graph.hpp
 * @brief DAG 기반 파이프라인 그래프 — PipelineGraph<T>
 * @defgroup qbuem_pipeline_graph PipelineGraph
 * @ingroup qbuem_pipeline
 *
 * PipelineGraph는 유향 비순환 그래프(DAG)로 파이프라인을 구성합니다.
 * fan-out (1→N), fan-in (N→1), A/B 라우팅을 지원합니다.
 *
 * ## 사용 예시
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
 * @brief 파이프라인 그래프의 노드 메타데이터.
 */
struct GraphNode {
    std::string name;          ///< 노드 이름 (고유)
    size_t      workers   = 1; ///< 워커 수
    size_t      chan_cap  = 256; ///< 채널 용량
    bool        enabled   = true; ///< 활성화 여부
};

/**
 * @brief 파이프라인 그래프의 엣지.
 */
struct GraphEdge {
    std::string from; ///< 출발 노드 이름
    std::string to;   ///< 도착 노드 이름
    /// @brief 선택적 라우팅 술어 (nullptr = 항상 라우팅)
    std::function<bool(const std::any& item)> predicate;
};

/**
 * @brief DAG 기반 파이프라인 그래프.
 *
 * fan-out(1→N), fan-in(N→1), A/B 라우팅을 지원합니다.
 *
 * @tparam T 그래프를 흐르는 메시지 타입.
 */
template <typename T>
class PipelineGraph {
public:
    /**
     * @brief PipelineGraph를 생성합니다.
     * @param default_chan_cap 기본 채널 용량.
     * @param registry 서비스 레지스트리 (nullptr이면 전역 레지스트리 사용).
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
    // 그래프 구성
    // -------------------------------------------------------------------------

    /**
     * @brief 처리 노드를 추가합니다.
     *
     * @tparam FnT ActionFn<FnT, T, T> concept을 만족하는 함수 타입.
     * @param name     노드 이름 (고유해야 함).
     * @param fn       처리 함수: T -> Task<Result<T>>.
     * @param workers  워커 수.
     * @param chan_cap 채널 용량 (0이면 default_chan_cap 사용).
     * @returns *this (메서드 체이닝용).
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
     * @brief from → to 엣지를 추가합니다 (fan-out 가능).
     *
     * @param from 출발 노드 이름.
     * @param to   도착 노드 이름.
     * @returns *this.
     */
    PipelineGraph& edge(std::string from, std::string to) {
        auto it = nodes_.find(from);
        if (it != nodes_.end()) {
            it->second->successors.push_back(to);
            it->second->predicates.push_back(nullptr); // 항상 라우팅
        }
        auto to_it = nodes_.find(to);
        if (to_it != nodes_.end()) {
            to_it->second->predecessors.push_back(from);
        }
        return *this;
    }

    /**
     * @brief 조건부 엣지를 추가합니다 (A/B 라우팅).
     *
     * @param from      출발 노드 이름.
     * @param to        도착 노드 이름.
     * @param predicate 라우팅 조건 함수.
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
     * @brief 노드를 진입점으로 표시합니다.
     *
     * @param node_name 소스 노드 이름.
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
     * @brief 노드를 출구점으로 표시합니다.
     *
     * @param node_name 싱크 노드 이름.
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
    // 라이프사이클
    // -------------------------------------------------------------------------

    /**
     * @brief 위상 정렬 후 모든 노드를 시작합니다 (Kahn's 알고리즘).
     *
     * @param dispatcher 코루틴을 실행할 Dispatcher.
     * @returns 사이클이 없으면 true, 사이클 감지 시 false.
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
     * @brief 아이템을 모든 소스 노드에 전송합니다 (backpressure).
     *
     * @param item 처리할 아이템.
     * @param ctx  아이템 컨텍스트.
     * @returns Result<void>::ok() 또는 에러.
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
     * @brief 논블로킹 push.
     *
     * @returns 모든 소스에 성공적으로 전송 시 true.
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
     * @brief 모든 소스를 닫고 모든 노드가 처리를 완료할 때까지 기다립니다.
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
     * @brief 즉시 정지합니다.
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
     * @brief 모든 싱크 노드의 병합 출력 채널을 반환합니다.
     */
    [[nodiscard]] std::shared_ptr<AsyncChannel<ContextualItem<T>>> output() const {
        return output_channel_;
    }

    // -------------------------------------------------------------------------
    // 토폴로지 내보내기
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
    // 런타임 검증
    // -------------------------------------------------------------------------

    /**
     * @brief 그래프 유효성을 검사합니다.
     *
     * - 모든 엣지의 노드가 존재하는지 확인
     * - 연결되지 않은 노드 확인
     * - 소스/싱크 노드가 존재하는지 확인
     *
     * @returns 유효하면 true.
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
    // 내부 노드 구조체
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
    // Kahn's 위상 정렬
    // -------------------------------------------------------------------------

    /**
     * @brief Kahn's 알고리즘으로 위상 정렬을 수행합니다.
     *
     * @returns 정렬된 노드 이름 목록. 사이클 감지 시 std::nullopt.
     */
    [[nodiscard]] std::optional<std::vector<std::string>> topo_sort() const {
        // 진입 차수 계산
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
    // 워커 코루틴
    // -------------------------------------------------------------------------

    /**
     * @brief 단일 노드 워커 코루틴.
     *
     * 입력 채널에서 아이템을 읽어 fn을 적용하고 출력 채널에 전달합니다.
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
     * @brief fan-out 워커 코루틴.
     *
     * 노드의 out_channel에서 읽어 모든 후속 노드의 in_channel에 배포합니다.
     * 조건부 술어가 있으면 평가 후 라우팅합니다.
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
     * @brief 싱크 노드의 출력 채널을 통합 출력 채널로 병합합니다.
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
    // 코루틴 yield 헬퍼
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
    // 데이터 멤버
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
