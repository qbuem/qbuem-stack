/**
 * @file bench/bench_http_parser.cpp
 * @brief HTTP/1.1 parser throughput benchmark.
 *
 * ### Measured Items
 * - HttpParser: simple GET request parse throughput (MB/s, ops/s)
 * - HttpParser: POST request with 10 headers parse throughput
 * - HttpParser: chunked transfer encoding parsing
 * - HttpParser: streaming parse (split receive simulation)
 *
 * ### Performance Goals (v1.0)
 * - Parser throughput (GET)  : > 300 MB/s
 * - Parser throughput (POST) : > 200 MB/s
 */

#include "bench_common.hpp"

#include <qbuem/http/parser.hpp>
#include <qbuem/http/request.hpp>

#include <print>
#include <string>

// ─── Test HTTP messages ───────────────────────────────────────────────────────

static const std::string kSimpleGet =
    "GET /api/v1/health HTTP/1.1\r\n"
    "Host: example.com\r\n"
    "Connection: keep-alive\r\n"
    "\r\n";

static const std::string kPostWithHeaders =
    "POST /api/v1/users HTTP/1.1\r\n"
    "Host: example.com\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 23\r\n"
    "Authorization: Bearer tok\r\n"
    "Accept: application/json\r\n"
    "Accept-Language: en-US\r\n"
    "X-Request-ID: abc123def456\r\n"
    "X-Forwarded-For: 192.168.1.1\r\n"
    "Cache-Control: no-cache\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "{\"name\":\"test\",\"age\":25}";

static const std::string kChunked =
    "POST /upload HTTP/1.1\r\n"
    "Host: example.com\r\n"
    "Transfer-Encoding: chunked\r\n"
    "\r\n"
    "5\r\n"
    "Hello\r\n"
    "6\r\n"
    " World\r\n"
    "0\r\n"
    "\r\n";

static const std::string kLargeHeaders =
    "GET /api/v1/data HTTP/1.1\r\n"
    "Host: api.example.com\r\n"
    "Authorization: Bearer eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9.payload.sig\r\n"
    "Accept: application/json, text/plain, */*\r\n"
    "Accept-Language: en-US,en;q=0.9\r\n"
    "Accept-Encoding: gzip, deflate, br\r\n"
    "X-Request-ID: 550e8400-e29b-41d4-a716-446655440000\r\n"
    "X-Correlation-ID: corr-abc123\r\n"
    "X-Forwarded-For: 203.0.113.1, 198.51.100.5\r\n"
    "X-Real-IP: 203.0.113.1\r\n"
    "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)\r\n"
    "Cache-Control: no-cache, no-store, must-revalidate\r\n"
    "Pragma: no-cache\r\n"
    "Connection: keep-alive\r\n"
    "\r\n";

// ─── Benchmark functions ──────────────────────────────────────────────────────

static void bench_parser_simple_get() {
    bench::section("HTTP Parser — GET (Simple Request)");

    std::println("  Request size: {} bytes", kSimpleGet.size());

    constexpr uint64_t kWarmup  = 10'000;
    constexpr uint64_t kIter    = 500'000;

    auto res = bench::run_batch(
        "HttpParser: simple GET",
        1,
        kWarmup, kIter,
        [&]() {
            qbuem::HttpParser parser;
            qbuem::Request    req;
            auto n = parser.parse(kSimpleGet, req);
            bench::do_not_optimize(n);
        }
    );
    res.print_throughput(kSimpleGet.size());

    const double mb_s = res.mb_per_sec(kSimpleGet.size());
    if (mb_s >= 300.0) {
        bench::pass("Throughput goal met: >= 300 MB/s");
    } else if (mb_s >= 100.0) {
        bench::pass("Throughput basic goal met: >= 100 MB/s");
    } else {
        bench::fail("Throughput goal missed: < 100 MB/s");
    }
}

static void bench_parser_post_with_headers() {
    bench::section("HTTP Parser — POST (10 Headers + JSON Body)");

    std::println("  Request size: {} bytes", kPostWithHeaders.size());

    constexpr uint64_t kWarmup  = 5'000;
    constexpr uint64_t kIter    = 300'000;

    auto res = bench::run_batch(
        "HttpParser: POST + 10 headers",
        1,
        kWarmup, kIter,
        [&]() {
            qbuem::HttpParser parser;
            qbuem::Request    req;
            auto n = parser.parse(kPostWithHeaders, req);
            bench::do_not_optimize(n);
        }
    );
    res.print_throughput(kPostWithHeaders.size());
}

static void bench_parser_chunked() {
    bench::section("HTTP Parser — Chunked Transfer Encoding");

    std::println("  Request size: {} bytes", kChunked.size());

    constexpr uint64_t kWarmup  = 5'000;
    constexpr uint64_t kIter    = 200'000;

    auto res = bench::run_batch(
        "HttpParser: chunked POST",
        1,
        kWarmup, kIter,
        [&]() {
            qbuem::HttpParser parser;
            qbuem::Request    req;
            auto n = parser.parse(kChunked, req);
            bench::do_not_optimize(n);
        }
    );
    res.print_throughput(kChunked.size());
}

static void bench_parser_large_headers() {
    bench::section("HTTP Parser — Large Headers GET (13 headers)");

    std::println("  Request size: {} bytes", kLargeHeaders.size());

    constexpr uint64_t kWarmup  = 5'000;
    constexpr uint64_t kIter    = 200'000;

    auto res = bench::run_batch(
        "HttpParser: GET + 13 headers",
        1,
        kWarmup, kIter,
        [&]() {
            qbuem::HttpParser parser;
            qbuem::Request    req;
            auto n = parser.parse(kLargeHeaders, req);
            bench::do_not_optimize(n);
        }
    );
    res.print_throughput(kLargeHeaders.size());
}

static void bench_parser_streaming() {
    bench::section("HTTP Parser — Streaming Parse (Split Receive)");

    // byte-by-byte parsing (worst case)
    constexpr uint64_t kWarmup = 1'000;
    constexpr uint64_t kIter   = 10'000;

    {
        auto res = bench::run_batch(
            "HttpParser: byte-by-byte (worst case)",
            kSimpleGet.size(),
            kWarmup, kIter,
            [&]() {
                qbuem::HttpParser parser;
                qbuem::Request    req;
                for (size_t i = 0; i < kSimpleGet.size(); ++i) {
                    auto n = parser.parse(kSimpleGet.substr(i, 1), req);
                    bench::do_not_optimize(n);
                }
            }
        );
        res.print();
    }

    // 16-byte chunk parsing (typical TCP recv case)
    {
        constexpr size_t kChunkSize = 16;
        auto res = bench::run_batch(
            "HttpParser: 16-byte chunks",
            kSimpleGet.size() / kChunkSize + 1,
            kWarmup, kIter * 5,
            [&]() {
                qbuem::HttpParser parser;
                qbuem::Request    req;
                for (size_t i = 0; i < kSimpleGet.size(); i += kChunkSize) {
                    auto chunk = kSimpleGet.substr(i, kChunkSize);
                    auto n = parser.parse(chunk, req);
                    bench::do_not_optimize(n);
                }
            }
        );
        res.print();
    }
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    std::println();
    std::println("══════════════════════════════════════════════════════════════");
    std::println("  qbuem-stack — HTTP Parser Performance Benchmark");
    std::println("══════════════════════════════════════════════════════════════");

    bench_parser_simple_get();
    bench_parser_post_with_headers();
    bench_parser_chunked();
    bench_parser_large_headers();
    bench_parser_streaming();

    std::println();
    std::println("══════════════════════════════════════════════════════════════");
    std::println("  Done");
    std::println("══════════════════════════════════════════════════════════════");
    std::println();

    return 0;
}
