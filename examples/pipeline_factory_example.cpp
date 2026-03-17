/**
 * @file pipeline_factory_example.cpp
 * @brief Config-driven 파이프라인 팩토리 예제 (qbuem-json 기반).
 *
 * ## 커버리지
 * - PipelineFactory<T>                — 팩토리 생성
 * - PipelineFactory::register_plugin()— 이름 → 액션 함수 매핑
 * - PipelineFactory::from_json()      — JSON 설정으로 DynamicPipeline 생성
 * - PipelineFactory::graph_from_json()— JSON 설정으로 PipelineGraph 생성
 * - detail::Json::value(key, default) — qbuem-json 으로 config 필드 파싱
 * - DynamicPipeline::start()          — 파이프라인 시작
 * - DynamicPipeline::try_push()       — 아이템 투입
 * - DynamicPipeline::output()         — 출력 채널
 *
 * ## JSON config 파싱 흐름
 *   from_json(json_str)
 *     → pipeline_factory.hpp 내부에서 qbuem_json 으로 파싱
 *     → 각 stage 의 "config" 서브-오브젝트를 detail::Json 으로 래핑
 *     → register_plugin 콜백(detail::Json& cfg) 전달
 *     → cfg.value("factor", 2)  ← qbuem_json Value::as<int>() 사용
 */

#include <qbuem_json/qbuem_json.hpp>
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/pipeline/action_env.hpp>
#include <qbuem/pipeline/pipeline_factory.hpp>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace qbuem;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// §1  선형 파이프라인 팩토리 — "config" 값을 qbuem-json 으로 파싱
// ─────────────────────────────────────────────────────────────────────────────

static void demo_linear_factory() {
    std::printf("── §1  선형 PipelineFactory (qbuem-json config 파싱) ──\n");

    PipelineFactory<int> factory;

    // 플러그인 등록: "multiplier" — cfg.value("factor", 2) 로 배수 결정
    factory.register_plugin("multiplier", [](const detail::Json& cfg) {
        int factor = cfg.value("factor", 2);   // JSON "config":{"factor":3} 파싱
        std::printf("  [multiplier] factor=%d (from config)\n", factor);
        return [factor](int x, ActionEnv) -> Task<Result<int>> {
            co_return x * factor;
        };
    });

    // 플러그인 등록: "adder" — cfg.value("offset", 10) 로 덧셈 값 결정
    factory.register_plugin("adder", [](const detail::Json& cfg) {
        int offset = cfg.value("offset", 10);  // JSON "config":{"offset":5} 파싱
        std::printf("  [adder]      offset=%d (from config)\n", offset);
        return [offset](int x, ActionEnv) -> Task<Result<int>> {
            co_return x + offset;
        };
    });

    // 플러그인 등록: "logger" — config 없음
    factory.register_plugin("logger", [](const detail::Json&) {
        return [](int x, ActionEnv) -> Task<Result<int>> {
            std::printf("  [logger] 값: %d\n", x);
            co_return x;
        };
    });

    // JSON config 으로 파이프라인 조립.
    //   multiplier: factor=3  (기본값 2 가 아닌 3 이 사용돼야 함)
    //   adder:      offset=5  (기본값 10 이 아닌 5 가 사용돼야 함)
    //   → 입력 1,2,3 → 출력 1*3+5=8, 2*3+5=11, 3*3+5=14
    auto dp = factory.from_json(R"({
        "type": "linear",
        "stages": [
            { "name": "x3",   "plugin": "multiplier", "config": { "factor":  3 } },
            { "name": "+5",   "plugin": "adder",       "config": { "offset":  5 } },
            { "name": "log",  "plugin": "logger" }
        ]
    })");

    if (!dp) {
        std::printf("  팩토리에서 파이프라인 생성 실패\n\n");
        return;
    }

    Dispatcher disp(1);
    std::thread t([&] { disp.run(); });

    dp->start(disp);

    // 입력: 1, 2, 3 → 기대 출력: 8, 11, 14 (factor=3, offset=5)
    for (int i = 1; i <= 3; ++i)
        dp->try_push(i);

    // 출력 수집
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

    std::printf("  결과: ");
    for (int r : results) std::printf("%d ", r);
    std::printf("  (기대값: 8 11 14)\n\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  플러그인 목록 조회 + string config 파싱
// ─────────────────────────────────────────────────────────────────────────────

static void demo_plugin_registry() {
    std::printf("── §2  플러그인 레지스트리 + string config ──\n");

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

    // "prefix" 플러그인 — config 에서 "text" 키로 접두사 결정
    factory.register_plugin("prefix", [](const detail::Json& cfg) {
        std::string text = cfg.value("text", std::string{"[qbuem] "});
        std::printf("  [prefix] text=\"%s\" (from config)\n", text.c_str());
        return [text](std::string s, ActionEnv) -> Task<Result<std::string>> {
            co_return text + s;
        };
    });

    std::printf("  플러그인 존재 확인: trim=%s upper=%s prefix=%s\n",
                factory.has_plugin("trim") ? "yes" : "no",
                factory.has_plugin("upper") ? "yes" : "no",
                factory.has_plugin("prefix") ? "yes" : "no");

    // "prefix" 의 config text 를 JSON 으로 주입
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
        std::printf("  파이프라인 생성 실패\n\n");
        return;
    }

    Dispatcher disp(1);
    std::thread t([&] { disp.run(); });
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

    std::printf("  입력: \"  hello world  \"\n");
    std::printf("  출력: \"%s\"\n\n", result.c_str());
}

// ─────────────────────────────────────────────────────────────────────────────
// §3  PipelineGraph 팩토리 — numeric config 파싱
// ─────────────────────────────────────────────────────────────────────────────

static void demo_graph_factory() {
    std::printf("── §3  PipelineGraph 팩토리 (numeric config) ──\n");

    PipelineFactory<int> factory;

    // "threshold" config 로 유효성 범위를 설정
    factory.register_plugin("validator", [](const detail::Json& cfg) {
        int min_val = cfg.value("min", 0);
        std::printf("  [validator] min=%d (from config)\n", min_val);
        return [min_val](int x, ActionEnv) -> Task<Result<int>> {
            if (x < min_val) co_return unexpected(
                std::make_error_code(std::errc::invalid_argument));
            co_return x;
        };
    });

    // "scale" config 로 배수 결정
    factory.register_plugin("doubler", [](const detail::Json& cfg) {
        int scale = cfg.value("scale", 2);
        std::printf("  [doubler]   scale=%d (from config)\n", scale);
        return [scale](int x, ActionEnv) -> Task<Result<int>> {
            co_return x * scale;
        };
    });

    factory.register_plugin("sink_print", [](const detail::Json&) {
        return [](int x, ActionEnv) -> Task<Result<int>> {
            std::printf("  [graph-sink] 값: %d\n", x);
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
        std::printf("  그래프 파이프라인 생성 실패\n\n");
        return;
    }

    Dispatcher disp(1);
    std::thread t([&] { disp.run(); });
    graph->start(disp);

    // 입력: -1(거부), 1, 2, 3 → sink 출력: 3, 6, 9 (scale=3)
    for (int i = -1; i <= 3; ++i)
        graph->try_push(i);

    std::this_thread::sleep_for(200ms);

    graph->stop();
    disp.stop();
    t.join();

    std::printf("  그래프 파이프라인 완료 (scale=3 → 출력: 3 6 9)\n\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::printf("=== qbuem PipelineFactory 예제 (qbuem-json config 파싱) ===\n\n");

    demo_linear_factory();
    demo_plugin_registry();
    demo_graph_factory();

    std::printf("=== 완료 ===\n");
    return 0;
}
