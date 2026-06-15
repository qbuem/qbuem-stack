// libFuzzer harness: Base64 / Base64url decode (untrusted encoded input).
#include <qbuem/crypto/base64.hpp>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view s(reinterpret_cast<const char*>(data), size);
    (void)qbuem::crypto::base64_decode(s);
    (void)qbuem::crypto::base64url_decode(s);
    return 0;
}
