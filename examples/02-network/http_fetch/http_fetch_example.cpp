/**
 * @file http_fetch_example.cpp
 * @brief qbuem monadic HTTP fetch — curl-free HTTP/1.1 client examples.
 *
 * Demonstrates the following patterns:
 *
 *   1. Basic GET request
 *   2. POST request with JSON body
 *   3. Monadic chaining — Result::map() / and_then() / value_or()
 *   4. Error handling — transform_error() / value_or()
 *   5. Request timeout
 *   6. Automatic redirect following
 *   7. URL parser direct usage
 *   8. FetchClient — connection pooling + Keep-Alive
 *
 * To run, point the URLs at a reachable HTTP server.
 * Replace "httpbin.org" with "127.0.0.1:8080" if no network access is available.
 */

#include <qbuem/http/fetch.hpp>
#include <qbuem/http/fetch_client.hpp>
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>

#include <chrono>
#include <print>
#include <stop_token>
#include <string_view>

using namespace qbuem;
using namespace std::chrono_literals;

// ─── Example 1: Basic GET ─────────────────────────────────────────────────────

Task<void> example_basic_get(std::stop_token st) {
    std::println("[1] Basic GET request");

    auto resp = co_await fetch("http://httpbin.org/get").send(st);

    if (!resp) {
        std::println("  error: {}", resp.error().message());
        co_return;
    }

    std::println("  status:       {}", resp->status());
    std::println("  ok():         {}", resp->ok());
    std::println("  content-type: {}", resp->header("content-type"));
    std::println("  body length:  {} bytes", resp->body().size());
    std::println("  body (first 200 chars):\n{}", resp->body().substr(0, 200));
}

// ─── Example 2: POST with JSON body ──────────────────────────────────────────

Task<void> example_post(std::stop_token st) {
    std::println("\n[2] POST request with JSON body");

    constexpr std::string_view payload = R"({"library":"qbuem","version":"2.2.0"})";

    auto resp = co_await fetch("http://httpbin.org/post")
        .post()
        .header("Content-Type", "application/json")
        .header("X-Request-ID", "fetch-example-002")
        .body(payload)
        .send(st);

    if (!resp) {
        std::println("  error: {}", resp.error().message());
        co_return;
    }

    std::println("  status: {}", resp->status());
    std::println("  body (first 300 chars):\n{}", resp->body().substr(0, 300));
}

// ─── Example 3: Monadic chaining ─────────────────────────────────────────────

Task<void> example_monadic_chain(std::stop_token st) {
    std::println("\n[3] Monadic chaining — map() / and_then() / value_or()");

    auto resp = co_await fetch("http://httpbin.org/status/200").send(st);

    // map(): transform success value (Result<FetchResponse> → Result<int>)
    auto status = resp.map([](const FetchResponse &r) { return r.status(); });
    std::println("  map() status: {}", status.value_or(-1));

    // and_then(): conditional transform (Result<FetchResponse> → Result<std::string>)
    auto body_or_err = resp.and_then([](const FetchResponse &r) -> Result<std::string> {
        if (!r.ok())
            return unexpected(std::make_error_code(std::errc::protocol_error));
        return std::string(r.body());
    });

    if (body_or_err)
        std::println("  and_then() body length: {}", body_or_err->size());
    else
        std::println("  and_then() error: {}", body_or_err.error().message());

    // Full chain: map → and_then → value_or
    std::string summary = resp
        .map([](const FetchResponse &r) {
            return std::string("status=") + std::to_string(r.status());
        })
        .and_then([](const std::string &s) -> Result<std::string> {
            return "summary: " + s;
        })
        .value_or("(error)");

    std::println("  chain result: {}", summary);
}

// ─── Example 4: Error handling ────────────────────────────────────────────────

Task<void> example_error_handling(std::stop_token st) {
    std::println("\n[4] Error handling — transform_error() / value_or()");

    // Non-existent host → connection error
    auto bad = co_await fetch("http://this-host-does-not-exist.qbuem/path")
        .send(st);

    // transform_error(): normalise / remap error codes
    auto normalized = bad.transform_error([](std::error_code ec) {
        std::println("  original error: {} ({})", ec.message(), ec.value());
        return std::make_error_code(std::errc::host_unreachable);
    });
    std::println("  normalised error: {}", normalized.error().message());

    // value_or(): safe fallback on error
    int fallback_status = bad
        .map([](const FetchResponse &r) { return r.status(); })
        .value_or(0);
    std::println("  value_or(0): {}", fallback_status);
}

// ─── Example 5: Timeout ───────────────────────────────────────────────────────

Task<void> example_timeout(std::stop_token st) {
    std::println("\n[5] Request timeout");

    // 1ms timeout — will almost certainly expire before the request completes
    auto resp = co_await fetch("http://httpbin.org/delay/2")
        .timeout(1ms)
        .send(st);

    if (!resp)
        std::println("  timed out (expected): {}", resp.error().message());
    else
        std::println("  completed with status: {} (timeout may not have fired)", resp->status());
}

// ─── Example 6: Redirect following ───────────────────────────────────────────

Task<void> example_redirect(std::stop_token st) {
    std::println("\n[6] Automatic redirect following");

    // /redirect/3 issues 3 × 302 redirects before responding 200
    auto resp = co_await fetch("http://httpbin.org/redirect/3")
        .max_redirects(5)
        .send(st);

    if (!resp) {
        std::println("  error: {}", resp.error().message());
        co_return;
    }
    std::println("  final status after 3 redirects: {}", resp->status());
    std::println("  final body length: {} bytes", resp->body().size());
}

// ─── Example 7: URL parser direct usage ──────────────────────────────────────

void example_url_parser() {
    std::println("\n[7] URL parser direct usage");

    struct TestCase { std::string_view url; };
    TestCase cases[] = {
        {"http://example.com/path?q=hello"},
        {"http://api.example.com:8080/v1/users"},
        {"https://secure.example.com/login"},
        {"http://[::1]:9090/grpc"},
        {"ftp://invalid-scheme"},
        {"not-a-url"},
    };

    for (auto &tc : cases) {
        auto r = ParsedUrl::parse(tc.url);
        if (r)
            std::println("  {} → scheme={} host={} port={} path={}",
                         tc.url, r->scheme, r->host, r->port, r->path);
        else
            std::println("  {} → parse failed: {}", tc.url, r.error().message());
    }
}

// ─── Example 8: FetchClient — connection pool + Keep-Alive ───────────────────

Task<void> example_fetch_client(std::stop_token st) {
    std::println("\n[8] FetchClient — connection pooling + Keep-Alive");

    FetchClient client;
    client.set_max_idle_per_host(4);
    client.set_timeout(10s);
    client.set_max_redirects(3);

    // First request: DNS + TCP handshake (cold)
    auto r1 = co_await client.request("http://httpbin.org/get").send(st);
    if (!r1) {
        std::println("  request 1 error: {}", r1.error().message());
        co_return;
    }
    std::println("  request 1 status: {} (cold — new connection)", r1->status());
    std::println("  idle connections after req 1: {}", client.idle_count());

    // Second request to same host: may reuse the pooled connection
    auto r2 = co_await client.request("http://httpbin.org/uuid").get().send(st);
    if (!r2) {
        std::println("  request 2 error: {}", r2.error().message());
        co_return;
    }
    std::println("  request 2 status: {} (may reuse connection)", r2->status());
    std::println("  idle connections after req 2: {}", client.idle_count());

    // Monadic chaining on client responses works exactly like fetch()
    auto uuid = r2
        .map([](const FetchResponse &r) { return std::string(r.body()); })
        .value_or("(error)");
    std::println("  body (first 60 chars): {}", uuid.substr(0, 60));

    // POST via client
    auto r3 = co_await client.request("http://httpbin.org/post")
        .post()
        .header("Content-Type", "application/json")
        .body(R"({"source":"fetch_client_example"})")
        .send(st);
    std::println("  POST status: {}", r3.map([](auto &r){ return r.status(); }).value_or(-1));
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
    co_await example_timeout(st);
    co_await example_redirect(st);
    co_await example_fetch_client(st);

    std::println("\nAll examples completed.");
}

int main() {
    Dispatcher dispatcher;
    dispatcher.run(run_all());
    return 0;
}
