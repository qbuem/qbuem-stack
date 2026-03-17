/**
 * @file http_fetch_example.cpp
 * @brief qbuem monadic HTTP fetch 사용 예제 — curl-free HTTP/1.1 클라이언트
 *
 * 이 예제는 다음 패턴들을 시연합니다:
 *
 *   1. 기본 GET 요청
 *   2. POST 요청 (JSON body)
 *   3. Monadic 체이닝 — Result::map() / and_then()
 *   4. 에러 핸들링 — transform_error() / value_or()
 *   5. URL 파싱 직접 사용
 *
 * 실행 전에 httpbin.org 또는 로컬 HTTP 서버가 필요합니다.
 * 네트워크가 없으면 URL을 127.0.0.1:8080 같은 로컬 서버로 바꾸세요.
 */

#include <qbuem/http/fetch.hpp>
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>

#include <print>
#include <stop_token>
#include <string_view>

using namespace qbuem;

// ─── 예제 1: 기본 GET 요청 ────────────────────────────────────────────────────

Task<void> example_basic_get(std::stop_token st) {
    std::println("[1] 기본 GET 요청");

    auto resp = co_await fetch("http://httpbin.org/get").send(st);

    if (!resp) {
        std::println("  오류: {}", resp.error().message());
        co_return;
    }

    std::println("  상태 코드: {}", resp->status());
    std::println("  ok(): {}", resp->ok());
    std::println("  Content-Type: {}", resp->header("content-type"));
    std::println("  Body 길이: {} bytes", resp->body().size());
    std::println("  Body (처음 200자):\n{}", resp->body().substr(0, 200));
}

// ─── 예제 2: POST 요청 ────────────────────────────────────────────────────────

Task<void> example_post(std::stop_token st) {
    std::println("\n[2] POST 요청 (JSON body)");

    constexpr std::string_view payload = R"({"user":"qbuem","version":"2.2.0"})";

    auto resp = co_await fetch("http://httpbin.org/post")
        .post()
        .header("Content-Type", "application/json")
        .header("X-Request-ID", "fetch-example-001")
        .body(payload)
        .send(st);

    if (!resp) {
        std::println("  오류: {}", resp.error().message());
        co_return;
    }

    std::println("  상태 코드: {}", resp->status());
    std::println("  Body (처음 300자):\n{}", resp->body().substr(0, 300));
}

// ─── 예제 3: Monadic 체이닝 ──────────────────────────────────────────────────

Task<void> example_monadic_chain(std::stop_token st) {
    std::println("\n[3] Monadic 체이닝 — map() + and_then()");

    auto resp = co_await fetch("http://httpbin.org/status/200").send(st);

    // .map() — 성공값 변환 (Result<FetchResponse> → Result<int>)
    auto status = resp.map([](const FetchResponse &r) { return r.status(); });
    std::println("  map() 결과 status: {}", status.value_or(-1));

    // .and_then() — 조건부 변환 (Result<FetchResponse> → Result<std::string>)
    auto body_or_err = resp.and_then([](const FetchResponse &r) -> Result<std::string> {
        if (!r.ok())
            return unexpected(std::make_error_code(std::errc::protocol_error));
        return std::string(r.body());
    });

    if (body_or_err) {
        std::println("  and_then() body 길이: {}", body_or_err->size());
    } else {
        std::println("  and_then() 에러: {}", body_or_err.error().message());
    }

    // 체이닝: map → and_then → value_or
    std::string summary = resp
        .map([](const FetchResponse &r) {
            return std::string("status=") + std::to_string(r.status());
        })
        .and_then([](const std::string &s) -> Result<std::string> {
            return "summary: " + s;
        })
        .value_or("(error)");

    std::println("  체이닝 결과: {}", summary);
}

// ─── 예제 4: 에러 핸들링 ─────────────────────────────────────────────────────

Task<void> example_error_handling(std::stop_token st) {
    std::println("\n[4] 에러 핸들링 — transform_error() + value_or()");

    // 존재하지 않는 호스트 → 연결 에러
    auto bad = co_await fetch("http://this-host-does-not-exist.qbuem/path").send(st);

    // transform_error(): 에러 코드 변환
    auto normalized = bad.transform_error([](std::error_code ec) {
        std::println("  원본 에러: {} ({})", ec.message(), ec.value());
        return std::make_error_code(std::errc::host_unreachable);
    });

    std::println("  정규화된 에러: {}", normalized.error().message());

    // value_or(): 에러 시 기본값
    int fallback_status = bad
        .map([](const FetchResponse &r) { return r.status(); })
        .value_or(0);
    std::println("  value_or(0) 결과: {}", fallback_status);
}

// ─── 예제 5: URL 파싱 직접 사용 ──────────────────────────────────────────────

void example_url_parser() {
    std::println("\n[5] URL 파서 직접 사용");

    struct TestCase { std::string_view url; };
    TestCase cases[] = {
        {"http://example.com/path?q=1"},
        {"http://api.example.com:8080/v1/users"},
        {"https://secure.example.com/login"},
        {"http://[::1]:9090/grpc"},
        {"ftp://invalid-scheme"},
    };

    for (auto &tc : cases) {
        auto r = ParsedUrl::parse(tc.url);
        if (r) {
            std::println("  URL: {}", tc.url);
            std::println("    scheme={} host={} port={} path={}",
                         r->scheme, r->host, r->port, r->path);
        } else {
            std::println("  URL: {} → 파싱 실패: {}", tc.url, r.error().message());
        }
    }
}

// ─── main ─────────────────────────────────────────────────────────────────────

Task<void> run_all() {
    std::stop_source ss;
    auto st = ss.get_token();

    example_url_parser();

    co_await example_basic_get(st);
    co_await example_post(st);
    co_await example_monadic_chain(st);
    co_await example_error_handling(st);

    std::println("\n모든 예제 완료.");
}

int main() {
    Dispatcher dispatcher;
    dispatcher.run(run_all());
    return 0;
}
