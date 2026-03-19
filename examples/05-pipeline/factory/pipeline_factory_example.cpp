/**
 * @file pipeline_factory_example.cpp
 * @brief Config-driven pipeline factory example (qbuem-json based).
 *
 * ## Coverage
 * - PipelineFactory<T>                — factory creation
 * - PipelineFactory::register_plugin()— name → action function mapping
 * - PipelineFactory::from_json()      — build DynamicPipeline from JSON config
 * - PipelineFactory::graph_from_json()— build PipelineGraph from JSON config
 * - detail::Json::value(key, default) — parse config fields with qbuem-json
 * - DynamicPipeline::start()          — start pipeline
 * - DynamicPipeline::try_push()       — push item
 * - DynamicPipeline::output()         — output channel
 *
 * ## JSON config parsing flow
 *   from_json(json_str)
 *     → parse internally with qbuem_json inside pipeline_factory.hpp
 *     → wrap each stage "config" sub-object as detail::Json
 *     → pass register_plugin callback(detail::Json& cfg)
 *     → cfg.value("factor", 2)  ← uses qbuem_json Value::as<int>()
 */

#include <qbuem_json/qbuem_json.hpp>
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/pipeline_factory.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include <qbuem/compat/print.hpp>

using namespace qbuem;
using namespace std::chrono_literals;
using std::println;
using std::print;

// ─────────────────────────────────────────────────────────────────────────────
// §1  Linear pipeline factory — parse "config" values with qbuem-json
// ─────────────────────────────────────────────────────────────────────────────

static void demo_linear_factory() {
    println("── §1  Linear PipelineFactory (qbuem-json config parsing) ──");

    PipelineFactory<int> factory;

    // Register plugin "multiplier" — multiplier determined by cfg.value("factor", 2)
    factory.register_plugin("multiplier", [](const detail::Json& cfg) {
        int factor = cfg.value("factor", 2);   // parse JSON "config":{"factor":3}
        println("  [multiplier] factor={} (from config)", factor);
        return [factor](int x, ActionEnv) -> Task<Result<int>> {
            co_return x * factor;
        };
    });

    // Register plugin "adder" — addend determined by cfg.value("offset", 10)
    factory.register_plugin("adder", [](const detail::Json& cfg) {
        int offset = cfg.value("offset", 10);  // parse JSON "config":{"offset":5}
        println("  [adder]      offset={} (from config)", offset);
        return [offset](int x, ActionEnv) -> Task<Result<int>> {
            co_return x + offset;
        };
    });

    // Register plugin "logger" — no config
    factory.register_plugin("logger", [](const detail::Json&) {
        return [](int x, ActionEnv) -> Task<Result<int>> {
            println("  [logger] value: {}", x);
            co_return x;
        };
    });

    // Assemble pipeline from JSON config.
    //   multiplier: factor=3  (should use 3, not default 2)
    //   adder:      offset=5  (should use 5, not default 10)
    //   → input 1,2,3 → output 1*3+5=8, 2*3+5=11, 3*3+5=14
    auto dp = factory.from_json(R"({
        "type": "linear",
        "stages": [
            { "name": "x3",   "plugin": "multiplier", "config": { "factor":  3 } },
            { "name": "+5",   "plugin": "adder",       "config": { "offset":  5 } },
            { "name": "log",  "plugin": "logger" }
        ]
    })");

    if (!dp) {
        println("  Failed to build pipeline from factory\n");
        return;
    }

    Dispatcher disp(1);
    std::jthread t([&] { disp.run(); });

    dp->start(disp);

    // Input: 1, 2, 3 → expected output: 8, 11, 14 (factor=3, offset=5)
    for (int i = 1; i <= 3; ++i)
        dp->try_push(i);

    // Collect output
    auto out = dp->output();
    std::vector<int> results;
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (results.size() < 3 && std::chrono::steady_clock::now() < deadline) {
        if (out) {
            auto item = out->try_recv();
            if (item) results.push_back(item->value);
            else std::this_thread::sleep_for(10ms);
        }
    }

    dp->stop();
    disp.stop();
    t.join();

    print("  results:");
    for (int r : results) print(" {}", r);
    println("  (expected: 8 11 14)\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  Plugin registry + string config parsing
// ─────────────────────────────────────────────────────────────────────────────

static void demo_plugin_registry() {
    println("── §2  Plugin Registry + string config ──");

    PipelineFactory<std::string> factory;

    factory.register_plugin("upper", [](const detail::Json&) {
        return [](std::string s, ActionEnv) -> Task<Result<std::string>> {
            for (char& c : s) c = static_cast<char>(std::toupper(c));
            co_return s;
        };
    });

    factory.register_plugin("trim", [](const detail::Json&) {
        return [](std::string s, ActionEnv) -> Task<Result<std::string>> {
            size_t start = s.find_first_not_of(' ');
            size_t end   = s.find_last_not_of(' ');
            if (start == std::string::npos) co_return std::string{};
            co_return s.substr(start, end - start + 1);
        };
    });

    // "prefix" plugin — prefix text determined by config "text" key
    factory.register_plugin("prefix", [](const detail::Json& cfg) {
        std::string text = cfg.value("text", std::string{"[qbuem] "});
        println("  [prefix] text=\"{}\" (from config)", text);
        return [text](std::string s, ActionEnv) -> Task<Result<std::string>> {
            co_return text + s;
        };
    });

    println("  plugin presence: trim={} upper={} prefix={}",
                factory.has_plugin("trim") ? "yes" : "no",
                factory.has_plugin("upper") ? "yes" : "no",
                factory.has_plugin("prefix") ? "yes" : "no");

    // Inject config text for "prefix" via JSON
    auto dp = factory.from_json(R"({
        "type": "linear",
        "stages": [
            { "name": "trim-it",   "plugin": "trim" },
            { "name": "upper-it",  "plugin": "upper" },
            { "name": "prefix-it", "plugin": "prefix",
              "config": { "text": "[QBUEM-JSON] " } }
        ]
    })");

    if (!dp) {
        println("  Failed to build pipeline\n");
        return;
    }

    Dispatcher disp(1);
    std::jthread t([&] { disp.run(); });
    dp->start(disp);

    dp->try_push("  hello world  ");

    auto out = dp->output();
    std::string result;
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (result.empty() && std::chrono::steady_clock::now() < deadline) {
        if (out) {
            auto item = out->try_recv();
            if (item) result = item->value;
            else std::this_thread::sleep_for(10ms);
        }
    }

    dp->stop();
    disp.stop();
    t.join();

    println("  input:  \"  hello world  \"");
    println("  output: \"{}\"\n", result);
}

// ─────────────────────────────────────────────────────────────────────────────
// §3  PipelineGraph factory — numeric config parsing
// ─────────────────────────────────────────────────────────────────────────────

static void demo_graph_factory() {
    println("── §3  PipelineGraph Factory (numeric config) ──");

    PipelineFactory<int> factory;

    // Set validation range via "threshold" config
    factory.register_plugin("validator", [](const detail::Json& cfg) {
        int min_val = cfg.value("min", 0);
        println("  [validator] min={} (from config)", min_val);
        return [min_val](int x, ActionEnv) -> Task<Result<int>> {
            if (x < min_val) co_return unexpected(
                std::make_error_code(std::errc::invalid_argument));
            co_return x;
        };
    });

    // Multiplier determined by "scale" config
    factory.register_plugin("doubler", [](const detail::Json& cfg) {
        int scale = cfg.value("scale", 2);
        println("  [doubler]   scale={} (from config)", scale);
        return [scale](int x, ActionEnv) -> Task<Result<int>> {
            co_return x * scale;
        };
    });

    factory.register_plugin("sink_print", [](const detail::Json&) {
        return [](int x, ActionEnv) -> Task<Result<int>> {
            println("  [graph-sink] value: {}", x);
            co_return x;
        };
    });

    // DAG: validator(min=1) → doubler(scale=3) → sink_print
    auto graph = factory.graph_from_json(R"({
        "type": "graph",
        "nodes": [
            { "name": "V", "plugin": "validator", "config": { "min":   1 } },
            { "name": "D", "plugin": "doubler",   "config": { "scale": 3 } },
            { "name": "S", "plugin": "sink_print" }
        ],
        "edges": [["V","D"], ["D","S"]],
        "source": "V",
        "sink":   "S"
    })");

    if (!graph) {
        println("  Failed to build graph pipeline\n");
        return;
    }

    Dispatcher disp(1);
    std::jthread t([&] { disp.run(); });
    graph->start(disp);

    // Input: -1 (rejected), 1, 2, 3 → sink output: 3, 6, 9 (scale=3)
    for (int i = -1; i <= 3; ++i)
        graph->try_push(i);

    std::this_thread::sleep_for(200ms);

    graph->stop();
    disp.stop();
    t.join();

    println("  Graph pipeline done (scale=3 → output: 3 6 9)\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    println("=== qbuem PipelineFactory Example (qbuem-json config parsing) ===\n");

    demo_linear_factory();
    demo_plugin_registry();
    demo_graph_factory();

    println("=== Done ===");
    return 0;
}
