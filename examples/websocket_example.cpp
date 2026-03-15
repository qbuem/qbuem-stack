/**
 * @file examples/websocket_example.cpp
 * @brief WebSocket 핸들러 예시 — RFC 6455 핸드셰이크 + 프레임 인코딩/디코딩
 */
#include <qbuem/server/websocket_handler.hpp>
#include <qbuem/http/request.hpp>

#include <iostream>
#include <string>
#include <vector>

using namespace qbuem;

// ─── WebSocket 에코 핸들러 예시 ─────────────────────────────────────────────

void websocket_echo_example() {
    // WebSocket 핸드셰이크 키 계산 예시
    std::string client_key = "dGhlIHNhbXBsZSBub25jZQ=="; // RFC 6455 예시 키
    std::string accept_key = WebSocketHandler::compute_accept_key(client_key);

    std::cout << "[ws] Sec-WebSocket-Accept: " << accept_key << "\n";
    // 예상값: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=

    // 텍스트 프레임 인코딩 (서버→클라이언트, 마스킹 없음)
    WsFrame frame;
    frame.opcode = WsFrame::Opcode::Text;
    frame.fin    = true;
    frame.masked = false;

    std::string payload = "Hello, WebSocket!";
    frame.payload.assign(
        reinterpret_cast<const uint8_t*>(payload.data()),
        reinterpret_cast<const uint8_t*>(payload.data()) + payload.size());

    auto encoded = WebSocketHandler::encode_frame(frame);
    std::cout << "[ws] Encoded frame bytes: " << encoded.size() << "\n";

    // 프레임 디코딩 (수신 측)
    WsFrame decoded;
    auto status = WebSocketHandler::decode_frame(encoded, decoded);
    if (status == WsDecodeStatus::Complete) {
        std::string msg(
            reinterpret_cast<const char*>(decoded.payload.data()),
            decoded.payload.size());
        std::cout << "[ws] Decoded: opcode=" << static_cast<int>(decoded.opcode)
                  << " fin=" << decoded.fin
                  << " payload='" << msg << "'\n";
    }

    // Ping/Pong 프레임
    WsFrame ping;
    ping.opcode = WsFrame::Opcode::Ping;
    ping.fin    = true;
    ping.masked = false;

    auto ping_encoded = WebSocketHandler::encode_frame(ping);
    std::cout << "[ws] Ping frame bytes: " << ping_encoded.size() << "\n";

    // Close 프레임
    WsFrame close;
    close.opcode = WsFrame::Opcode::Close;
    close.fin    = true;
    close.masked = false;
    // Close 프레임에는 선택적으로 상태 코드(1000=Normal) 포함 가능
    uint16_t code = htons(1000);
    close.payload.assign(reinterpret_cast<const uint8_t*>(&code),
                         reinterpret_cast<const uint8_t*>(&code) + 2);

    auto close_encoded = WebSocketHandler::encode_frame(close);
    std::cout << "[ws] Close frame bytes: " << close_encoded.size() << "\n";
}

// ─── 마스킹 예시 (클라이언트→서버) ─────────────────────────────────────────

void masked_frame_example() {
    // 클라이언트 → 서버 방향은 반드시 마스킹 필요 (RFC 6455 §5.3)
    WsFrame masked_frame;
    masked_frame.opcode = WsFrame::Opcode::Binary;
    masked_frame.fin    = true;
    masked_frame.masked = true;

    std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
    masked_frame.payload = data;

    auto encoded = WebSocketHandler::encode_frame(masked_frame);
    std::cout << "[ws] Masked binary frame: " << encoded.size() << " bytes\n";

    // 디코딩 — 서버에서 언마스킹
    WsFrame decoded;
    auto status = WebSocketHandler::decode_frame(encoded, decoded);
    if (status == WsDecodeStatus::Complete) {
        std::cout << "[ws] Unmasked payload: ";
        for (auto b : decoded.payload)
            std::cout << std::hex << (int)b << " ";
        std::cout << std::dec << "\n";
    }
}

// ─── HTTP 업그레이드 요청 검증 ──────────────────────────────────────────────

void upgrade_validation_example() {
    // HTTP/1.1 Upgrade 요청 헤더 검증
    // 실제로는 Request 객체에서 헤더를 읽지만, 여기서는 직접 체크
    bool is_upgrade    = true;  // "Upgrade: websocket" 헤더 존재
    bool has_ws_key    = true;  // "Sec-WebSocket-Key" 헤더 존재
    bool has_ws_ver    = true;  // "Sec-WebSocket-Version: 13" 헤더 존재

    if (is_upgrade && has_ws_key && has_ws_ver) {
        std::cout << "[ws] WebSocket handshake validated\n";
    }
}

int main() {
    websocket_echo_example();
    masked_frame_example();
    upgrade_validation_example();
    return 0;
}
