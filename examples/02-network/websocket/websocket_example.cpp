/**
 * @file examples/websocket_example.cpp
 * @brief WebSocket handler example — RFC 6455 handshake + frame encode/decode.
 */
#include <qbuem/server/websocket_handler.hpp>
#include <qbuem/http/request.hpp>

#include <arpa/inet.h>
#include <string>
#include <vector>
#include <qbuem/compat/print.hpp>

using namespace qbuem;
using std::println;
using std::print;

// ─── WebSocket echo handler example ─────────────────────────────────────────

void websocket_echo_example() {
    // WebSocket handshake key computation example
    std::string client_key = "dGhlIHNhbXBsZSBub25jZQ=="; // RFC 6455 example key
    std::string accept_key = WebSocketHandler::compute_accept_key(client_key);

    println("[ws] Sec-WebSocket-Accept: {}", accept_key);
    // Expected: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=

    // Text frame encoding (server→client, no masking)
    WsFrame frame;
    frame.opcode = WsFrame::Opcode::Text;
    frame.fin    = true;
    frame.masked = false;

    std::string payload = "Hello, WebSocket!";
    frame.payload.assign(
        reinterpret_cast<const uint8_t*>(payload.data()),
        reinterpret_cast<const uint8_t*>(payload.data()) + payload.size());

    auto encoded = WebSocketHandler::encode_frame(frame);
    println("[ws] Encoded frame bytes: {}", encoded.size());

    // Frame decoding (receiver side) — decode_frame returns Result<WsFrame>
    size_t consumed = 0;
    auto decode_result = WebSocketHandler::decode_frame(
        std::span<const uint8_t>{encoded.data(), encoded.size()}, consumed);
    if (decode_result) {
        const WsFrame& decoded = *decode_result;
        std::string msg(
            reinterpret_cast<const char*>(decoded.payload.data()),
            decoded.payload.size());
        println("[ws] Decoded: opcode={} fin={} payload='{}'",
                static_cast<int>(decoded.opcode), decoded.fin, msg);
    }

    // Ping/Pong frame
    WsFrame ping;
    ping.opcode = WsFrame::Opcode::Ping;
    ping.fin    = true;
    ping.masked = false;

    auto ping_encoded = WebSocketHandler::encode_frame(ping);
    println("[ws] Ping frame bytes: {}", ping_encoded.size());

    // Close frame
    WsFrame close;
    close.opcode = WsFrame::Opcode::Close;
    close.fin    = true;
    close.masked = false;
    // Close frame may optionally include a status code (1000 = Normal)
    uint16_t code = htons(1000);
    close.payload.assign(reinterpret_cast<const uint8_t*>(&code),
                         reinterpret_cast<const uint8_t*>(&code) + 2);

    auto close_encoded = WebSocketHandler::encode_frame(close);
    println("[ws] Close frame bytes: {}", close_encoded.size());
}

// ─── Masked frame example (client→server) ───────────────────────────────────

void masked_frame_example() {
    // Client → server direction requires masking (RFC 6455 §5.3)
    WsFrame masked_frame;
    masked_frame.opcode = WsFrame::Opcode::Binary;
    masked_frame.fin    = true;
    masked_frame.masked = true;

    std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
    masked_frame.payload = data;

    auto encoded = WebSocketHandler::encode_frame(masked_frame);
    println("[ws] Masked binary frame: {} bytes", encoded.size());

    // Decoding — server unmasks the payload
    size_t consumed2 = 0;
    auto decode_result2 = WebSocketHandler::decode_frame(
        std::span<const uint8_t>{encoded.data(), encoded.size()}, consumed2);
    if (decode_result2) {
        print("[ws] Unmasked payload:");
        for (auto b : decode_result2->payload)
            print(" {:x}", static_cast<int>(b));
        println("");
    }
}

// ─── HTTP upgrade request validation ────────────────────────────────────────

void upgrade_validation_example() {
    // HTTP/1.1 Upgrade request header validation
    // In practice, headers are read from a Request object; checked directly here
    bool is_upgrade    = true;  // "Upgrade: websocket" header present
    bool has_ws_key    = true;  // "Sec-WebSocket-Key" header present
    bool has_ws_ver    = true;  // "Sec-WebSocket-Version: 13" header present

    if (is_upgrade && has_ws_key && has_ws_ver) {
        println("[ws] WebSocket handshake validated");
    }
}

int main() {
    websocket_echo_example();
    masked_frame_example();
    upgrade_validation_example();
    return 0;
}
