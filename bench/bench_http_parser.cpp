/**
 * @file bench/bench_http_parser.cpp
 * @brief HTTP/1.1 파서 처리량 벤치마크.
 *
 * ### 측정 항목
 * - HttpParser: 단순 GET 요청 파싱 처리량 (MB/s, ops/s)
 * - HttpParser: 헤더 10개 POST 요청 파싱 처리량
 * - HttpParser: chunked transfer encoding 파싱
 * - HttpParser: 스트리밍 파싱 (분할 수신 시뮬레이션)
 *
 * ### 성능 목표 (v1.0)
 * - 파서 처리량 (GET)  : > 300 MB/s
 * - 파서 처리량 (POST) : > 200 MB/s
 */

#include "bench_common.hpp"

#include <qbuem/http/parser.hpp>
#include <qbuem/http/request.hpp>

#include <string>

// ─── 테스트 HTTP 메시지 ───────────────────────────────────────────────────────

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
    "Accept-Language: ko-KR\r\n"
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
    "Accept-Language: ko-KR,ko;q=0.9,en-US;q=0.8,en;q=0.7\r\n"
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

// ─── 벤치마크 함수들 ──────────────────────────────────────────────────────────

static void bench_parser_simple_get() {
    bench::section("HTTP Parser — GET (단순 요청)");

    printf("  요청 크기: %zu bytes\n", kSimpleGet.size());

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
        bench::pass("처리량 목표 달성: >= 300 MB/s");
    } else if (mb_s >= 100.0) {
        bench::pass("처리량 기본 달성: >= 100 MB/s");
    } else {
        bench::fail("처리량 목표 미달: < 100 MB/s");
    }
}

static void bench_parser_post_with_headers() {
    bench::section("HTTP Parser — POST (헤더 10개 + JSON body)");

    printf("  요청 크기: %zu bytes\n", kPostWithHeaders.size());

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

    printf("  요청 크기: %zu bytes\n", kChunked.size());

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
    bench::section("HTTP Parser — 대형 헤더 GET (13개 헤더)");

    printf("  요청 크기: %zu bytes\n", kLargeHeaders.size());

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
    bench::section("HTTP Parser — 스트리밍 파싱 (분할 수신)");

    // 1바이트씩 파싱 (최악 케이스)
    constexpr uint64_t kWarmup = 1'000;
    constexpr uint64_t kIter   = 10'000;

    {
        auto res = bench::run_batch(
            "HttpParser: byte-by-byte (최악 케이스)",
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

    // 16바이트 청크로 파싱 (일반적인 TCP recv 케이스)
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

// ─── 메인 ────────────────────────────────────────────────────────────────────

int main() {
    printf("\n");
    printf("══════════════════════════════════════════════════════════════\n");
    printf("  qbuem-stack v1.0.0 — HTTP Parser 성능 벤치마크\n");
    printf("══════════════════════════════════════════════════════════════\n");

    bench_parser_simple_get();
    bench_parser_post_with_headers();
    bench_parser_chunked();
    bench_parser_large_headers();
    bench_parser_streaming();

    printf("\n══════════════════════════════════════════════════════════════\n");
    printf("  완료\n");
    printf("══════════════════════════════════════════════════════════════\n\n");

    return 0;
}
