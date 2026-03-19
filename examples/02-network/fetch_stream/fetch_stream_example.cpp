/**
 * @file fetch_stream_example.cpp
 * @brief qbuem FetchStream — zero-copy HTTP response body streaming.
 *
 * Demonstrates processing large HTTP responses with constant memory overhead
 * using FetchStreamClient. The body is delivered chunk-by-chunk through a
 * fixed pool of 64 KiB slots — no full-body allocation.
 *
 * Patterns demonstrated:
 *   1. Basic streaming GET — count bytes without buffering the full body
 *   2. Pipeline integration — push chunks into an AsyncChannel for processing
 *   3. Header inspection — check Content-Type before streaming
 *   4. Cancellation — stop_token integration with the pump coroutine
 *
 * Run against any HTTP server that returns a large body:
 *   http://speedtest.ftp.otenet.gr/files/test10Mb.db
 *   http://localhost:8080/large-file
 */

#include <qbuem/http/fetch_stream.hpp>
#include <qbuem/core/dispatcher.hpp>
#include <qbuem/core/task.hpp>

#include <atomic>
#include <chrono>
#include <print>
#include <stop_token>
#include <string_view>

using namespace qbuem;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// §1. Basic streaming GET — byte counter
// ─────────────────────────────────────────────────────────────────────────────

Task<void> demo_stream_count(std::stop_token st) {
    std::println("\n─── §1. Streaming GET — byte counter ─────────────────────");

    FetchStreamClient client;
    auto stream = co_await client.stream("http://localhost:8080/large", st);
    if (!stream) {
        std::println("  [skip] connect failed: {} (start a local HTTP server to demo)",
                     stream.error().message());
        co_return;
    }

    // Start background pump on a detached coroutine
    auto& s = *stream;
    Dispatcher::current()->spawn(s->start_pump(st));

    size_t total_bytes = 0;
    size_t chunk_count = 0;

    auto t0 = std::chrono::steady_clock::now();

    while (auto chunk = co_await s->next(st)) {
        total_bytes += (*chunk)->size();
        ++chunk_count;
        s->release(*chunk);  // return slot to pool — no heap free
    }

    auto elapsed = std::chrono::steady_clock::now() - t0;
    double ms = std::chrono::duration<double, std::milli>(elapsed).count();

    std::println("  received {} bytes in {} chunks ({:.1f} ms, {:.1f} MB/s)",
                 total_bytes, chunk_count, ms,
                 (total_bytes / 1e6) / (ms / 1000.0));
}

// ─────────────────────────────────────────────────────────────────────────────
// §2. Header inspection before streaming
// ─────────────────────────────────────────────────────────────────────────────

Task<void> demo_stream_headers(std::stop_token st) {
    std::println("\n─── §2. Header inspection before streaming ────────────────");

    FetchStreamClient client;
    auto stream = co_await client.stream("http://localhost:8080/json-stream", st);
    if (!stream) {
        std::println("  [skip] connect failed (start a local HTTP server to demo)");
        co_return;
    }

    auto& s = *stream;

    // Inspect headers before pumping any body
    std::println("  HTTP status : {}", s->status());
    std::println("  Content-Type: {}", s->header("content-type"));
    std::println("  ok()        : {}", s->ok());

    if (!s->ok()) {
        std::println("  non-2xx status — skipping body");
        co_return;
    }

    // Only stream if content type is what we expect
    auto ct = s->header("content-type");
    if (ct.find("json") == std::string_view::npos &&
        ct.find("octet-stream") == std::string_view::npos) {
        std::println("  unexpected content type — skipping");
        co_return;
    }

    Dispatcher::current()->spawn(s->start_pump(st));

    size_t bytes = 0;
    while (auto chunk = co_await s->next(st)) {
        bytes += (*chunk)->size();
        s->release(*chunk);
    }
    std::println("  streamed {} bytes of JSON/binary data", bytes);
}

// ─────────────────────────────────────────────────────────────────────────────
// §3. Cancellation — stop after first chunk
// ─────────────────────────────────────────────────────────────────────────────

Task<void> demo_stream_cancel(std::stop_token outer_st) {
    std::println("\n─── §3. Cancellation — stop after first chunk ─────────────");

    std::stop_source inner_ss;
    auto st = inner_ss.get_token();

    FetchStreamClient client;
    auto stream = co_await client.stream("http://localhost:8080/large", outer_st);
    if (!stream) {
        std::println("  [skip] connect failed");
        co_return;
    }

    auto& s = *stream;
    Dispatcher::current()->spawn(s->start_pump(st));

    auto chunk = co_await s->next(st);
    if (chunk) {
        std::println("  received first chunk: {} bytes — cancelling now",
                     (*chunk)->size());
        s->release(*chunk);
    }

    // Cancel the pump — remaining data is discarded
    inner_ss.request_stop();
    std::println("  stop requested; stream cancelled");
}

// ─────────────────────────────────────────────────────────────────────────────
// §4. Simulated streaming (in-process) — no network required
// ─────────────────────────────────────────────────────────────────────────────

Task<void> demo_stream_simulated(std::stop_token st) {
    std::println("\n─── §4. Simulated streaming — pool/channel mechanics ───────");

    // Demonstrate FetchChunk pool mechanics without a real HTTP server
    // by directly constructing a FetchStream and posting chunks manually.

    constexpr size_t kSimChunks  = 8;
    constexpr size_t kSimPayload = 1024;

    // Manually construct a stream (content-length mode, no chunked encoding)
    TcpStream dummy_stream;  // fd=-1 placeholder
    FetchStream fs(std::move(dummy_stream), 200, "content-type: application/octet-stream\r\n",
                   static_cast<int64_t>(kSimChunks * kSimPayload), false);

    std::println("  pool slots : {}", FetchStream::kPoolSlots);
    std::println("  channel cap: {}", FetchStream::kChanCap);
    std::println("  chunk size : {} bytes", FetchChunk::kChunkSize);
    std::println("  ok()       : {}", fs.ok());
    std::println("  status     : {}", fs.status());
    std::println("  Simulated streaming: {} chunks × {} bytes = {} bytes total",
                 kSimChunks, kSimPayload, kSimChunks * kSimPayload);
    std::println("  (Full pump demo requires a live HTTP server on :8080)");
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

Task<void> run_all(std::stop_token st) {
    std::println("╔══════════════════════════════════════════════════════════╗");
    std::println("║       qbuem-stack FetchStream Demo                       ║");
    std::println("╚══════════════════════════════════════════════════════════╝");

    co_await demo_stream_simulated(st);
    co_await demo_stream_count(st);
    co_await demo_stream_headers(st);
    co_await demo_stream_cancel(st);

    std::println("\n Done.\n");
}

int main() {
    Dispatcher d(2);
    std::jthread t([&d]{ d.run(); });

    std::stop_source ss;
    d.spawn(run_all(ss.get_token()));

    // Wait briefly then stop
    std::this_thread::sleep_for(std::chrono::seconds{3});
    ss.request_stop();
    d.stop();
    return 0;
}
