// libFuzzer harness: URL parser (ParsedUrl::parse) — parses untrusted URLs.
#include <qbuem/http/fetch.hpp>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    (void)qbuem::ParsedUrl::parse(
        std::string_view(reinterpret_cast<const char*>(data), size));
    return 0;
}
