// libFuzzer harness: WebSocket frame decoder (untrusted client frames, masking).
#include <qbuem/server/websocket_handler.hpp>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::span<const uint8_t> in(data, size);
    size_t consumed = 0;
    // Drain repeatedly: each decode advances by `consumed` over the buffer.
    size_t off = 0;
    for (int i = 0; i < 64 && off < size; ++i) {
        std::span<const uint8_t> sub = in.subspan(off);
        consumed = 0;
        auto r = qbuem::WebSocketHandler::decode_frame(sub, consumed);
        if (!r || consumed == 0) break;
        off += consumed;
    }
    return 0;
}
