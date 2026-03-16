/**
 * @file pipeline_factory_example.cpp
 * @brief Config-driven 파이프라인 팩토리 예제.
 *
 * ## 커버리지
 * - PipelineFactory<T>                — 팩토리 생성
 * - PipelineFactory::register_plugin()— 이름 → 액션 함수 매핑
 * - PipelineFactory::from_json()      — JSON 설정으로 DynamicPipeline 생성
 * - PipelineFactory::graph_from_json()— JSON 설정으로 PipelineGraph 생성
 * - DynamicPipeline::start()          — 파이프라인 시작
 * - DynamicPipeline::try_push()       — 아이템 투입
 * - DynamicPipeline::output()         — 출력 채널
 */

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
// §1  선형 파이프라인 팩토리
// ─────────────────────────────────────────────────────────────────────────────

static void demo_linear_factory() {
    std::printf("── §1  선형 PipelineFactory ──\n");

    PipelineFactory<int> factory;

    // 플러그인 등록: "multiplier" — factor만큼 곱하기
    factory.register_plugin("multiplier", [](const detail::Json& cfg) {
        int factor = cfg.value("factor", 2);
        return [factor](int x, ActionEnv) -> Task<Result<int>> {
            co_return x * factor;
        };
    });

    // 플러그인 등록: "adder" — offset만큼 더하기
    factory.register_plugin("adder", [](const detail::Json& cfg) {
        int offset = cfg.value("offset", 10);
        return [offset](int x, ActionEnv) -> Task<Result<int>> {
            co_return x + offset;
        };
    });

    // 플러그인 등록: "logger" — 값 출력
    factory.register_plugin("logger", [](const detail::Json&) {
        return [](int x, ActionEnv) -> Task<Result<int>> {
            std::printf("  [logger] 값: %d\n", x);
            co_return x;
        };
    });

    // JSON 설정으로 파이프라인 조립
    // from_json은 내부적으로 nlohmann/json이 없으면 플러그인 이름만 사용
    // (config 필드는 무시되고 기본값 사용)
    auto dp = factory.from_json(R"({
        "type": "linear",
        "stages": [
            { "name": "x3",   "plugin": "multiplier" },
            { "name": "+10",  "plugin": "adder" },
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

    // 입력: 1, 2, 3 → 출력: 1*2+10=12, 2*2+10=14, 3*2+10=16 (기본 factor=2)
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
    std::printf("\n\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  플러그인 목록 조회
// ─────────────────────────────────────────────────────────────────────────────

static void demo_plugin_registry() {
    std::printf("── §2  플러그인 레지스트리 ──\n");

    PipelineFactory<std::string> factory;

    factory.register_plugin("upper", [](const detail::Json&) {
        return [](std::string s, ActionEnv) -> Task<Result<std::string>> {
            for (char& c : s) c = static_cast<char>(std::toupper(c));
            co_return s;
        };
    });

    factory.register_plugin("trim", [](const detail::Json&) {
        return [](std::string s, ActionEnv) -> Task<Result<std::string>> {
            // 앞뒤 공백 제거
            size_t start = s.find_first_not_of(' ');
            size_t end   = s.find_last_not_of(' ');
            if (start == std::string::npos) co_return std::string{};
            co_return s.substr(start, end - start + 1);
        };
    });

    factory.register_plugin("prefix", [](const detail::Json&) {
        return [](std::string s, ActionEnv) -> Task<Result<std::string>> {
            co_return "[qbuem] " + s;
        };
    });

    std::printf("  플러그인 존재 확인: trim=%s upper=%s prefix=%s\n",
                factory.has_plugin("trim") ? "yes" : "no",
                factory.has_plugin("upper") ? "yes" : "no",
                factory.has_plugin("prefix") ? "yes" : "no");

    // 파이프라인 조립
    auto dp = factory.from_json(R"({
        "type": "linear",
        "stages": [
            { "name": "trim-it",   "plugin": "trim" },
            { "name": "upper-it",  "plugin": "upper" },
            { "name": "prefix-it", "plugin": "prefix" }
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
// §3  PipelineGraph 팩토리
// ─────────────────────────────────────────────────────────────────────────────

static void demo_graph_factory() {
    std::printf("── §3  PipelineGraph 팩토리 ──\n");

    PipelineFactory<int> factory;

    factory.register_plugin("validator", [](const detail::Json&) {
        return [](int x, ActionEnv) -> Task<Result<int>> {
            if (x < 0) co_return unexpected(
                std::make_error_code(std::errc::invalid_argument));
            co_return x;
        };
    });

    factory.register_plugin("doubler", [](const detail::Json&) {
        return [](int x, ActionEnv) -> Task<Result<int>> {
            co_return x * 2;
        };
    });

    factory.register_plugin("sink_print", [](const detail::Json&) {
        return [](int x, ActionEnv) -> Task<Result<int>> {
            std::printf("  [graph-sink] 값: %d\n", x);
            co_return x;
        };
    });

    // DAG 구성: validator → doubler → sink_print
    auto graph = factory.graph_from_json(R"({
        "type": "graph",
        "nodes": [
            { "name": "V", "plugin": "validator" },
            { "name": "D", "plugin": "doubler" },
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

    for (int i = 1; i <= 3; ++i)
        graph->try_push(i);

    std::this_thread::sleep_for(200ms);

    graph->stop();
    disp.stop();
    t.join();

    std::printf("  그래프 파이프라인 완료\n\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::printf("=== qbuem PipelineFactory 예제 ===\n\n");

    demo_linear_factory();
    demo_plugin_registry();
    demo_graph_factory();

    std::printf("=== 완료 ===\n");
    return 0;
}
