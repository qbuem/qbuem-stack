// libFuzzer harness: length-prefixed frame decode (untrusted length header + body).
#include <qbuem/codec/length_prefix_codec.hpp>
#include <qbuem/common.hpp>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    qbuem::codec::LengthPrefixedCodec codec;
    qbuem::BufferView buf(data, size);
    qbuem::codec::LengthPrefixedFrame frame;
    for (int i = 0; i < 64; ++i) {
        auto st = codec.decode(buf, frame);
        if (st != qbuem::codec::DecodeStatus::Complete) break;
    }
    return 0;
}
