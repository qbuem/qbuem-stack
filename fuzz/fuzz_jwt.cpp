// libFuzzer harness: SIMD JWT parser (untrusted token splitting + claim reads).
#include <qbuem/security/simd_jwt.hpp>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    qbuem::security::SIMDJwtParser parser;
    auto view = parser.parse(std::string_view(reinterpret_cast<const char*>(data), size));
    (void)view;
    return 0;
}
