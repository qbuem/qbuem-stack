// libFuzzer harness: line codec incremental decode over untrusted bytes.
#include <qbuem/codec/line_codec.hpp>
#include <qbuem/common.hpp>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    qbuem::codec::LineCodec codec;
    qbuem::BufferView buf(data, size);
    qbuem::codec::Line line;
    // Drain repeatedly until no more complete frames (decode advances `buf`).
    for (int i = 0; i < 64; ++i) {
        auto st = codec.decode(buf, line);
        if (st != qbuem::codec::DecodeStatus::Complete) break;
    }
    return 0;
}
