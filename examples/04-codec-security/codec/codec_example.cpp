/**
 * @file examples/codec_example.cpp
 * @brief Codec 예시 — LengthPrefixedCodec + LineCodec
 */
#include <qbuem/codec/length_prefix_codec.hpp>
#include <qbuem/codec/line_codec.hpp>
#include <qbuem/codec/frame_codec.hpp>

#include <cstddef>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/uio.h>
#include <vector>

using namespace qbuem::codec;
using qbuem::BufferView;

// ─── LengthPrefixedCodec 예시 ───────────────────────────────────────────────

void length_prefix_example() {
    std::cout << "=== LengthPrefixedCodec ===\n";

    LengthPrefixedCodec codec;

    // 1. 인코딩: "Hello Protocol!" 메시지 직렬화
    std::string message = "Hello Protocol!";
    LengthPrefixedFrame send_frame;
    send_frame.payload.assign(
        reinterpret_cast<const std::byte*>(message.data()),
        reinterpret_cast<const std::byte*>(message.data()) + message.size());
    send_frame.length = static_cast<uint32_t>(message.size());

    iovec vecs[2];
    size_t n = codec.encode(send_frame, vecs, 2, nullptr);
    // vecs[0] = 4바이트 빅엔디안 길이, vecs[1] = 페이로드
    size_t total = 0;
    for (size_t i = 0; i < n; ++i)
        total += vecs[i].iov_len;
    std::cout << "[length_prefix] Encoded " << total << " bytes (4 header + "
              << message.size() << " payload)\n";

    // 2. 직렬화 → 원시 바이트 버퍼 생성
    std::vector<uint8_t> wire;
    for (size_t i = 0; i < n; ++i) {
        const auto* p = static_cast<const uint8_t*>(vecs[i].iov_base);
        wire.insert(wire.end(), p, p + vecs[i].iov_len);
    }

    // 3. 디코딩: 수신 버퍼에서 프레임 복원
    LengthPrefixedCodec recv_codec;
    LengthPrefixedFrame recv_frame;

    BufferView buf{wire.data(), wire.size()};
    auto status = recv_codec.decode(buf, recv_frame);

    if (status == DecodeStatus::Complete) {
        std::string decoded(
            reinterpret_cast<const char*>(recv_frame.payload.data()),
            recv_frame.payload.size());
        std::cout << "[length_prefix] Decoded: '" << decoded << "'\n";
        recv_codec.reset();
    }

    // 4. 부분 수신 시뮬레이션 (스트리밍)
    LengthPrefixedCodec partial_codec;
    LengthPrefixedFrame partial_frame;

    // 처음 3바이트만 제공 (헤더 4바이트 중 3바이트)
    BufferView partial{wire.data(), 3};
    auto partial_status = partial_codec.decode(partial, partial_frame);
    std::cout << "[length_prefix] Partial (3 bytes): "
              << (partial_status == DecodeStatus::Incomplete ? "Incomplete" : "Error") << "\n";

    // 나머지 바이트 추가 제공
    BufferView rest{wire.data() + 3, wire.size() - 3};
    auto rest_status = partial_codec.decode(rest, partial_frame);
    std::cout << "[length_prefix] After rest: "
              << (rest_status == DecodeStatus::Complete ? "Complete" : "Incomplete") << "\n";
}

// ─── LineCodec 예시 ─────────────────────────────────────────────────────────

void line_codec_example() {
    std::cout << "\n=== LineCodec (CRLF) ===\n";

    // CRLF 모드 (Redis RESP, HTTP 헤더 등)
    LineCodec crlf_codec(true);

    std::string raw = "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";
    std::vector<uint8_t> data(raw.begin(), raw.end());

    // 모든 라인 디코딩
    size_t line_count = 0;
    while (true) {
        BufferView view{data.data(), data.size()};
        Line line;
        auto s = crlf_codec.decode(view, line);
        if (s != DecodeStatus::Complete) break;

        size_t consumed = data.size() - view.size();
        std::string content(line.data.begin(), line.data.end());
        std::cout << "[line] Line " << ++line_count << ": '" << content << "'\n";
        data.erase(data.begin(), data.begin() + static_cast<ptrdiff_t>(consumed));
        crlf_codec.reset();
        if (content.empty()) break; // 빈 라인 = 헤더 끝
    }

    std::cout << "[line] Total lines decoded: " << line_count << "\n";

    // LF 모드 (Unix 로그 등)
    LineCodec lf_codec(false);
    std::cout << "\n=== LineCodec (LF-only) ===\n";

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
        std::cout << "[lf_line] " << content << "\n";
        log_bytes.erase(log_bytes.begin(), log_bytes.begin() + static_cast<ptrdiff_t>(consumed));
        lf_codec.reset();
        ++log_lines;
    }
    std::cout << "[lf_line] Total: " << log_lines << " log lines\n";
}

int main() {
    length_prefix_example();
    line_codec_example();
    return 0;
}
