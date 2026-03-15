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
#include <vector>

using namespace qbuem::codec;

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

    auto iov = codec.encode(send_frame, nullptr);
    // iov[0] = 4바이트 빅엔디안 길이, iov[1] = 페이로드
    size_t total = 0;
    for (size_t i = 0; i < iov.count; ++i)
        total += iov.vecs[i].iov_len;
    std::cout << "[length_prefix] Encoded " << total << " bytes (4 header + "
              << message.size() << " payload)\n";

    // 2. 직렬화 → 원시 바이트 버퍼 생성
    std::vector<std::byte> wire;
    for (size_t i = 0; i < iov.count; ++i) {
        const auto* p = static_cast<const std::byte*>(iov.vecs[i].iov_base);
        wire.insert(wire.end(), p, p + iov.vecs[i].iov_len);
    }

    // 3. 디코딩: 수신 버퍼에서 프레임 복원
    LengthPrefixedCodec recv_codec;
    LengthPrefixedFrame recv_frame;

    BufferView buf{reinterpret_cast<const uint8_t*>(wire.data()), wire.size()};
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
    BufferView partial{reinterpret_cast<const uint8_t*>(wire.data()), 3};
    auto partial_status = partial_codec.decode(partial, partial_frame);
    std::cout << "[length_prefix] Partial (3 bytes): "
              << (partial_status == DecodeStatus::Incomplete ? "Incomplete" : "Error") << "\n";

    // 나머지 바이트 추가 제공
    BufferView rest{reinterpret_cast<const uint8_t*>(wire.data() + 3), wire.size() - 3};
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
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(raw.data());
    size_t offset = 0;

    while (offset < raw.size()) {
        BufferView view{ptr + offset, raw.size() - offset};
        Line line;
        auto status = crlf_codec.decode(view, line);

        if (status == DecodeStatus::Complete) {
            if (line.data.empty()) {
                std::cout << "[line_codec] Empty line (header end)\n";
            } else {
                std::cout << "[line_codec] Line: '" << line.data << "'\n";
            }
            // 소비된 바이트 계산 (line.data 포인터 기반)
            size_t consumed = (line.data.data() - reinterpret_cast<const char*>(ptr + offset))
                              + line.data.size() + 2; // +2 for CRLF
            offset += consumed;
            crlf_codec.reset();
        } else {
            break; // Incomplete
        }
    }

    // LF 모드 (단순 텍스트)
    std::cout << "\n=== LineCodec (LF only) ===\n";
    LineCodec lf_codec(false);
    std::string lf_raw = "line1\nline2\nline3\n";
    const uint8_t* lf_ptr = reinterpret_cast<const uint8_t*>(lf_raw.data());
    size_t lf_offset = 0;

    while (lf_offset < lf_raw.size()) {
        BufferView lf_view{lf_ptr + lf_offset, lf_raw.size() - lf_offset};
        Line lf_line;
        auto s = lf_codec.decode(lf_view, lf_line);
        if (s == DecodeStatus::Complete) {
            std::cout << "[lf_codec] '" << lf_line.data << "'\n";
            size_t consumed = (lf_line.data.data() - reinterpret_cast<const char*>(lf_ptr + lf_offset))
                              + lf_line.data.size() + 1; // +1 for LF
            lf_offset += consumed;
            lf_codec.reset();
        } else {
            break;
        }
    }

    // 인코딩
    Line out_line;
    out_line.data = "PING";
    auto iov = crlf_codec.encode(out_line, nullptr);
    std::cout << "\n[line_codec] Encoded 'PING\\r\\n': segments=" << iov.count << "\n";
}

int main() {
    length_prefix_example();
    line_codec_example();
    return 0;
}
