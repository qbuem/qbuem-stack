/**
 * @file examples/codec_example.cpp
 * @brief Codec example — LengthPrefixedCodec + LineCodec
 */
#include <qbuem/codec/length_prefix_codec.hpp>
#include <qbuem/codec/line_codec.hpp>
#include <qbuem/codec/frame_codec.hpp>

#include <cstddef>
#include <cstring>
#include <string>
#include <sys/uio.h>
#include <vector>
#include <qbuem/compat/print.hpp>

using namespace qbuem::codec;
using qbuem::BufferView;
using std::println;
using std::print;

// ─── LengthPrefixedCodec example ─────────────────────────────────────────────

void length_prefix_example() {
    println("=== LengthPrefixedCodec ===");

    LengthPrefixedCodec codec;

    // 1. Encoding: serialize the message "Hello Protocol!"
    std::string message = "Hello Protocol!";
    LengthPrefixedFrame send_frame;
    send_frame.payload.assign(
        reinterpret_cast<const std::byte*>(message.data()),
        reinterpret_cast<const std::byte*>(message.data()) + message.size());
    send_frame.length = static_cast<uint32_t>(message.size());

    iovec vecs[2];
    size_t n = codec.encode(send_frame, vecs, 2, nullptr);
    // vecs[0] = 4-byte big-endian length, vecs[1] = payload
    size_t total = 0;
    for (size_t i = 0; i < n; ++i)
        total += vecs[i].iov_len;
    println("[length_prefix] Encoded {} bytes (4 header + {} payload)",
              total, message.size());

    // 2. Serialize → build raw byte buffer
    std::vector<uint8_t> wire;
    for (size_t i = 0; i < n; ++i) {
        const auto* p = static_cast<const uint8_t*>(vecs[i].iov_base);
        wire.insert(wire.end(), p, p + vecs[i].iov_len);
    }

    // 3. Decoding: restore frame from receive buffer
    LengthPrefixedCodec recv_codec;
    LengthPrefixedFrame recv_frame;

    BufferView buf{wire.data(), wire.size()};
    auto status = recv_codec.decode(buf, recv_frame);

    if (status == DecodeStatus::Complete) {
        std::string decoded(
            reinterpret_cast<const char*>(recv_frame.payload.data()),
            recv_frame.payload.size());
        println("[length_prefix] Decoded: '{}'", decoded);
        recv_codec.reset();
    }

    // 4. Partial receive simulation (streaming)
    LengthPrefixedCodec partial_codec;
    LengthPrefixedFrame partial_frame;

    // Supply only first 3 bytes (3 of 4 header bytes)
    BufferView partial{wire.data(), 3};
    auto partial_status = partial_codec.decode(partial, partial_frame);
    println("[length_prefix] Partial (3 bytes): {}",
              partial_status == DecodeStatus::Incomplete ? "Incomplete" : "Error");

    // Supply the remaining bytes
    BufferView rest{wire.data() + 3, wire.size() - 3};
    auto rest_status = partial_codec.decode(rest, partial_frame);
    println("[length_prefix] After rest: {}",
              rest_status == DecodeStatus::Complete ? "Complete" : "Incomplete");
}

// ─── LineCodec example ────────────────────────────────────────────────────────

void line_codec_example() {
    println("\n=== LineCodec (CRLF) ===");

    // CRLF mode (Redis RESP, HTTP headers, etc.)
    LineCodec crlf_codec(true);

    std::string raw = "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";
    std::vector<uint8_t> data(raw.begin(), raw.end());

    // Decode all lines
    size_t line_count = 0;
    while (true) {
        BufferView view{data.data(), data.size()};
        Line line;
        auto s = crlf_codec.decode(view, line);
        if (s != DecodeStatus::Complete) break;

        size_t consumed = data.size() - view.size();
        std::string content(line.data.begin(), line.data.end());
        println("[line] Line {}: '{}'", ++line_count, content);
        data.erase(data.begin(), data.begin() + static_cast<ptrdiff_t>(consumed));
        crlf_codec.reset();
        if (content.empty()) break; // empty line = end of headers
    }

    println("[line] Total lines decoded: {}", line_count);

    // LF mode (Unix logs, etc.)
    LineCodec lf_codec(false);
    println("\n=== LineCodec (LF-only) ===");

    std::string log_data = "INFO: server started\nWARN: high memory\nERROR: timeout\n";
    std::vector<uint8_t> log_bytes(log_data.begin(), log_data.end());

    size_t log_lines = 0;
    while (true) {
        BufferView lf_view{log_bytes.data(), log_bytes.size()};
        Line lf_line;
        auto lf_s = lf_codec.decode(lf_view, lf_line);
        if (lf_s != DecodeStatus::Complete) break;

        size_t consumed = log_bytes.size() - lf_view.size();
        std::string content(lf_line.data.begin(), lf_line.data.end());
        println("[lf_line] {}", content);
        log_bytes.erase(log_bytes.begin(), log_bytes.begin() + static_cast<ptrdiff_t>(consumed));
        lf_codec.reset();
        ++log_lines;
    }
    println("[lf_line] Total: {} log lines", log_lines);
}

int main() {
    length_prefix_example();
    line_codec_example();
    return 0;
}
