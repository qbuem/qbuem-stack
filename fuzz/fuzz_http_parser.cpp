// libFuzzer harness: HTTP/1.1 request parser (the primary untrusted-input surface).
// Build: clang++ -std=c++23 -fsanitize=fuzzer,address,undefined -Iinclude \
//          fuzz/fuzz_http_parser.cpp src/http/parser.cpp src/http/request.cpp \
//          src/http/response.cpp -o /tmp/fz_http
#include <qbuem/http/parser.hpp>
#include <qbuem/http/request.hpp>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    qbuem::HttpParser parser;
    qbuem::Request req;
    // Fresh parser per input; also feed in two halves to exercise incremental state.
    parser.parse(std::string_view(reinterpret_cast<const char*>(data), size), req);
    if (size > 1) {
        qbuem::HttpParser p2;
        qbuem::Request r2;
        size_t mid = size / 2;
        p2.parse(std::string_view(reinterpret_cast<const char*>(data), mid), r2);
        p2.parse(std::string_view(reinterpret_cast<const char*>(data) + mid, size - mid), r2);
    }
    return 0;
}
