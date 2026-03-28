/**
 * @file neon_validation.cpp
 * @brief Standalone NEON intrinsic validation — tests each NEON path against
 *        a scalar reference implementation with multiple input sizes and
 *        boundary conditions.
 *
 * Build:
 *   clang++ -std=c++23 -O2 -march=native -o /tmp/neon_validation \
 *       tests/neon_validation.cpp && /tmp/neon_validation
 *
 * Must be run on AArch64 (ARM64) hardware.
 */

#include <arm_neon.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Compatibility shim: <print> requires GCC 14+ / Clang 18+; provide println()
// via <format> + fputs for compilers that lack it (e.g. GCC 13 on CI).
#if __has_include(<print>)
#  include <print>
using std::println;
#else
namespace {
template <class... Args>
inline void println(std::format_string<Args...> fmt, Args&&... args) {
    std::string s = std::format(fmt, std::forward<Args>(args)...);
    s += '\n';
    std::fputs(s.c_str(), stdout);
}
inline void println(std::string_view sv) {
    std::fputs(sv.data(), stdout);
    std::fputc('\n', stdout);
}
}  // anonymous namespace
#endif

// ─── Test harness ─────────────────────────────────────────────────────────────

static int g_total = 0;
static int g_passed = 0;

static void check(bool ok, std::string_view name) {
    ++g_total;
    if (ok) {
        ++g_passed;
        println("  PASS  {}", name);
    } else {
        println("  FAIL  {}", name);
    }
}

// ─── 1. HTTP header scanner (\r\n\r\n) ────────────────────────────────────────

// Scalar reference
static size_t find_header_end_scalar(const char* data, size_t len) noexcept {
    if (len < 4) return SIZE_MAX;
    for (size_t i = 0; i + 3 < len; ++i) {
        if (data[i]=='\r' && data[i+1]=='\n' && data[i+2]=='\r' && data[i+3]=='\n')
            return i;
    }
    return SIZE_MAX;
}

// NEON implementation (from src/http/parser.cpp)
static size_t find_header_end_neon(const char* data, size_t len) noexcept {
    if (len < 4) return SIZE_MAX;

    const uint8x16_t v_cr = vdupq_n_u8(static_cast<uint8_t>('\r'));
    size_t i = 0;
    for (; i + 16 <= len; i += 16) {
        const uint8x16_t chunk = vld1q_u8(reinterpret_cast<const uint8_t*>(data + i));
        const uint8x16_t cmp   = vceqq_u8(chunk, v_cr);
        if (vmaxvq_u8(cmp) == 0) continue;

        const uint8x8_t  narrow = vshrn_n_u16(vreinterpretq_u16_u8(cmp), 4);
        uint64_t         mask64 = vget_lane_u64(vreinterpret_u64_u8(narrow), 0);

        while (mask64) {
            const int lane = __builtin_ctzll(mask64) >> 2;
            const size_t off = i + static_cast<size_t>(lane);
            if (off + 3 < len &&
                data[off+1] == '\n' && data[off+2] == '\r' && data[off+3] == '\n')
                return off;
            mask64 &= mask64 - 1;
        }
    }
    for (; i + 3 < len; ++i) {
        if (data[i]=='\r' && data[i+1]=='\n' && data[i+2]=='\r' && data[i+3]=='\n')
            return i;
    }
    return SIZE_MAX;
}

static void test_http_scanner() {
    println("\n[1] HTTP header scanner (\\r\\n\\r\\n)");

    // Basic hit in first 16-byte chunk
    {
        const char* req = "GET / HTTP/1.1\r\n\r\nBody";
        size_t len = strlen(req);
        size_t s = find_header_end_scalar(req, len);
        size_t n = find_header_end_neon(req, len);
        check(s == n, "basic GET: position matches scalar");
        check(n != SIZE_MAX, "basic GET: found");
    }

    // Hit exactly at byte 16 (crosses 16-byte boundary)
    {
        std::string s(12, 'A');
        s += "\r\n\r\nExtra";
        size_t sref = find_header_end_scalar(s.data(), s.size());
        size_t sneon = find_header_end_neon(s.data(), s.size());
        check(sref == sneon, "boundary at byte 12: position matches");
    }

    // Hit in second 16-byte chunk (offset 20)
    {
        std::string s(20, 'X');
        s += "\r\n\r\n";
        size_t sref = find_header_end_scalar(s.data(), s.size());
        size_t sneon = find_header_end_neon(s.data(), s.size());
        check(sref == sneon, "hit at offset 20: position matches");
        check(sneon == 20, "hit at offset 20: correct position");
    }

    // No terminator present
    {
        const char* req = "GET / HTTP/1.1\r\nHost: example.com\r\n";
        size_t len = strlen(req);
        check(find_header_end_neon(req, len) == SIZE_MAX, "no terminator: SIZE_MAX");
    }

    // False positive guard: single \r without \n\r\n
    {
        std::string s = "aaaa\rbbbb\r\n\r\n";
        size_t sref  = find_header_end_scalar(s.data(), s.size());
        size_t sneon = find_header_end_neon(s.data(), s.size());
        check(sref == sneon, "false positive guard: matches scalar");
    }

    // Large HTTP request (> 32 bytes, terminator at end)
    {
        std::string s = "POST /api/data HTTP/1.1\r\n"
                        "Host: example.com\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: 42\r\n"
                        "\r\n"
                        "{\"key\":\"value\"}";
        size_t sref  = find_header_end_scalar(s.data(), s.size());
        size_t sneon = find_header_end_neon(s.data(), s.size());
        check(sref == sneon, "full HTTP request: position matches scalar");
        check(sneon != SIZE_MAX, "full HTTP request: found");
    }

    // Minimum length (< 4)
    {
        check(find_header_end_neon("abc", 3) == SIZE_MAX, "len=3: SIZE_MAX");
        check(find_header_end_neon("", 0) == SIZE_MAX, "len=0: SIZE_MAX");
    }

    // Input exactly 16 bytes with terminator at last 4
    {
        std::string s(12, 'Z');
        s += "\r\n\r\n"; // 16 bytes total
        size_t sref  = find_header_end_scalar(s.data(), s.size());
        size_t sneon = find_header_end_neon(s.data(), s.size());
        check(sref == sneon && sref == 12, "exactly 16 bytes, hit at 12");
    }
}

// ─── 2. WebSocket XOR masking ─────────────────────────────────────────────────

// Scalar reference
static void xor_mask_scalar(const uint8_t* src, uint8_t* dst, size_t len,
                             const std::array<uint8_t, 4>& key) noexcept {
    for (size_t i = 0; i < len; ++i)
        dst[i] = src[i] ^ key[i % 4];
}

// NEON implementation (from include/qbuem/server/websocket_handler.hpp)
static void xor_mask_neon(const uint8_t* src, uint8_t* dst, size_t len,
                           const std::array<uint8_t, 4>& key) noexcept {
    alignas(16) uint8_t key16[16];
    for (int i = 0; i < 16; ++i) key16[i] = key[i & 3];
    const uint8x16_t vkey = vld1q_u8(key16);

    size_t i = 0;
    for (; i + 16 <= len; i += 16) {
        const uint8x16_t chunk = vld1q_u8(src + i);
        vst1q_u8(dst + i, veorq_u8(chunk, vkey));
    }
    for (; i < len; ++i)
        dst[i] = src[i] ^ key[i & 3];
}

static void test_ws_masking() {
    println("\n[2] WebSocket XOR masking");

    const std::array<uint8_t, 4> key = {0xAB, 0xCD, 0xEF, 0x12};

    auto run = [&](size_t len, std::string_view label) {
        std::vector<uint8_t> src(len);
        for (size_t i = 0; i < len; ++i) src[i] = static_cast<uint8_t>(i * 7 + 3);

        std::vector<uint8_t> out_scalar(len), out_neon(len);
        xor_mask_scalar(src.data(), out_scalar.data(), len, key);
        xor_mask_neon  (src.data(), out_neon.data(),   len, key);
        check(out_scalar == out_neon, label);
    };

    run(0,  "len=0: empty");
    run(1,  "len=1: single byte");
    run(4,  "len=4: exactly one key cycle");
    run(15, "len=15: one chunk minus 1");
    run(16, "len=16: exactly one NEON chunk");
    run(17, "len=17: one chunk + tail");
    run(31, "len=31: two chunks minus 1");
    run(32, "len=32: exactly two NEON chunks");
    run(63, "len=63: four chunks minus 1");
    run(64, "len=64: exactly four chunks");
    run(100,"len=100: non-multiple of 16");

    // In-place masking (src == dst)
    {
        std::vector<uint8_t> buf(32);
        for (size_t i = 0; i < 32; ++i) buf[i] = static_cast<uint8_t>(i);
        std::vector<uint8_t> ref = buf;
        xor_mask_scalar(ref.data(), ref.data(), 32, key);
        xor_mask_neon(buf.data(), buf.data(), 32, key);
        check(ref == buf, "in-place masking: correct");
    }

    // Double-masking recovers original (XOR idempotence)
    {
        std::vector<uint8_t> orig(48), masked(48), recovered(48);
        for (size_t i = 0; i < 48; ++i) orig[i] = static_cast<uint8_t>(i * 3);
        xor_mask_neon(orig.data(),   masked.data(),    48, key);
        xor_mask_neon(masked.data(), recovered.data(), 48, key);
        check(orig == recovered, "double-mask recovers original");
    }
}

// ─── 3. Constant-time comparison ─────────────────────────────────────────────

// Scalar reference
static bool ct_equal_scalar(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<uint8_t>(a[i]) ^ static_cast<uint8_t>(b[i]);
    return diff == 0;
}

// NEON implementation (from include/qbuem/crypto.hpp)
static bool ct_equal_neon(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    const size_t n  = a.size();
    const auto* pa = reinterpret_cast<const uint8_t*>(a.data());
    const auto* pb = reinterpret_cast<const uint8_t*>(b.data());

    uint8x16_t acc = vdupq_n_u8(0);
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        const uint8x16_t va = vld1q_u8(pa + i);
        const uint8x16_t vb = vld1q_u8(pb + i);
        acc = vorrq_u8(acc, veorq_u8(va, vb));
    }
    volatile uint8_t diff = vmaxvq_u8(acc);
    for (; i < n; ++i)
        diff |= pa[i] ^ pb[i];
    return diff == 0;
}

static void test_ct_compare() {
    println("\n[3] Constant-time comparison");

    auto run = [](std::string_view a, std::string_view b, bool expect, std::string_view label) {
        bool sref = ct_equal_scalar(a, b);
        bool sneon = ct_equal_neon(a, b);
        check(sref == expect && sneon == expect, label);
    };

    run("hello", "hello", true,  "equal 5-byte strings");
    run("hello", "world", false, "different 5-byte strings");
    run("", "", true,            "equal empty strings");
    run("a", "b", false,        "single-byte differ");
    run("a", "a", true,         "single-byte equal");

    // 16-byte strings (exactly one NEON vector)
    run("AAAAAAAAAAAAAAAA", "AAAAAAAAAAAAAAAA", true,  "16 bytes equal");
    run("AAAAAAAAAAAAAAAA", "AAAAAAAAAAAAAAAB", false, "16 bytes last byte differs");

    // 17-byte strings (16 NEON + 1 scalar tail)
    run("AAAAAAAAAAAAAAAAB", "AAAAAAAAAAAAAAAAB", true,  "17 bytes equal");
    run("AAAAAAAAAAAAAAAAB", "AAAAAAAAAAAAAAAAC", false, "17 bytes tail differs");

    // 32-byte strings
    {
        std::string s1(32, 'X'), s2(32, 'X');
        run(s1, s2, true, "32 bytes equal");
        s2[31] = 'Y';
        run(s1, s2, false, "32 bytes last byte differs");
        s2[0] = 'Z'; s2[31] = 'X';
        run(s1, s2, false, "32 bytes first byte differs");
    }

    // Different sizes
    run("abc", "abcd", false, "different sizes: false");
    run("", "x", false,       "empty vs non-empty: false");
}

// ─── 4. JWT dot scanner ───────────────────────────────────────────────────────

struct DotPositions { size_t first; size_t second; bool valid; };

// Scalar reference
static DotPositions find_dots_scalar(const char* data, size_t len) noexcept {
    size_t first = len;
    for (size_t i = 0; i < len; ++i)
        if (data[i] == '.') { first = i; break; }
    if (first >= len) return {len, len, false};
    size_t second = len;
    for (size_t i = first + 1; i < len; ++i)
        if (data[i] == '.') { second = i; break; }
    if (second >= len || first >= second) return {len, len, false};
    return {first, second, true};
}

// NEON implementation (from include/qbuem/security/simd_jwt.hpp)
static DotPositions find_dots_neon(const char* data, size_t len) noexcept {
    const uint8x16_t dot_vec = vdupq_n_u8(static_cast<uint8_t>('.'));

    size_t first = len;
    size_t i = 0;
    for (; i + 16 <= len && first == len; i += 16) {
        uint8x16_t chunk = vld1q_u8(reinterpret_cast<const uint8_t*>(data + i));
        uint8x16_t cmp   = vceqq_u8(chunk, dot_vec);
        if (vmaxvq_u8(cmp) != 0) {
            for (size_t j = i; j < i + 16 && first == len; ++j)
                if (data[j] == '.') first = j;
        }
    }
    for (; i < len && first == len; ++i)
        if (data[i] == '.') first = i;

    if (first >= len) return {len, len, false};

    size_t second = len;
    i = first + 1;
    for (; i + 16 <= len && second == len; i += 16) {
        uint8x16_t chunk = vld1q_u8(reinterpret_cast<const uint8_t*>(data + i));
        uint8x16_t cmp   = vceqq_u8(chunk, dot_vec);
        if (vmaxvq_u8(cmp) != 0) {
            for (size_t j = i; j < i + 16 && second == len; ++j)
                if (data[j] == '.') second = j;
        }
    }
    for (; i < len && second == len; ++i)
        if (data[i] == '.') second = i;

    if (second >= len || first >= second) return {len, len, false};
    return {first, second, true};
}

static void test_jwt_dots() {
    println("\n[4] JWT dot scanner");

    auto run = [](const char* token, std::string_view label) {
        size_t len = strlen(token);
        DotPositions s = find_dots_scalar(token, len);
        DotPositions n = find_dots_neon(token, len);
        bool ok = (s.valid == n.valid) &&
                  (!s.valid || (s.first == n.first && s.second == n.second));
        check(ok, label);
    };

    // Typical JWT (3 parts: header.payload.signature)
    run("eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJ1c2VyIn0.SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c",
        "typical JWT: dots found correctly");

    // Dots within first 16 bytes
    run("abc.def.ghi", "short JWT: dots in first chunk");

    // First dot at byte 16 (boundary)
    {
        std::string t(15, 'A');
        t += '.';
        t += std::string(10, 'B');
        t += '.';
        t += std::string(5, 'C');
        DotPositions s = find_dots_scalar(t.data(), t.size());
        DotPositions n = find_dots_neon(t.data(), t.size());
        check(s.first == n.first && s.second == n.second && n.valid,
              "first dot at boundary byte 15");
    }

    // First dot at byte 32 (second chunk boundary)
    {
        std::string t(31, 'A');
        t += '.';
        t += std::string(20, 'B');
        t += '.';
        t += std::string(10, 'C');
        DotPositions s = find_dots_scalar(t.data(), t.size());
        DotPositions n = find_dots_neon(t.data(), t.size());
        check(s.first == n.first && s.second == n.second && n.valid,
              "first dot at boundary byte 31");
    }

    // No dots → invalid
    run("nodots", "no dots: invalid");

    // Only one dot → invalid
    run("one.dot", "one dot: invalid");

    // Empty
    run("", "empty: invalid");

    // Dots at positions 0 and 1
    run("..", "adjacent dots at start");

    // Second dot immediately after first
    run("a..b", "adjacent dots: a..b");
}

// ─── 5. Base64url encoding ────────────────────────────────────────────────────

static constexpr char kB64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

// Scalar reference (RFC 4648 §5, no padding)
static std::string base64url_scalar(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve((len * 4 + 2) / 3);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b  = static_cast<uint8_t>(data[i]) << 16;
        if (i + 1 < len) b |= static_cast<uint8_t>(data[i+1]) << 8;
        if (i + 2 < len) b |= static_cast<uint8_t>(data[i+2]);
        out += kB64Table[(b >> 18) & 0x3F];
        out += kB64Table[(b >> 12) & 0x3F];
        if (i + 1 < len) out += kB64Table[(b >> 6) & 0x3F];
        if (i + 2 < len) out += kB64Table[(b     ) & 0x3F];
    }
    return out;
}

// NEON implementation (from include/qbuem/crypto.hpp)
static std::string base64url_neon(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve((len * 4 + 2) / 3);

    alignas(16) uint8_t tbl0[16], tbl1[16], tbl2[16], tbl3[16];
    for (int i = 0; i < 16; ++i) tbl0[i] = static_cast<uint8_t>(kB64Table[i]);
    for (int i = 0; i < 16; ++i) tbl1[i] = static_cast<uint8_t>(kB64Table[i + 16]);
    for (int i = 0; i < 16; ++i) tbl2[i] = static_cast<uint8_t>(kB64Table[i + 32]);
    for (int i = 0; i < 16; ++i) tbl3[i] = static_cast<uint8_t>(kB64Table[i + 48]);

    const uint8x16_t vtbl0 = vld1q_u8(tbl0);
    const uint8x16_t vtbl1 = vld1q_u8(tbl1);
    const uint8x16_t vtbl2 = vld1q_u8(tbl2);
    const uint8x16_t vtbl3 = vld1q_u8(tbl3);

    size_t i = 0;
    for (; i + 12 <= len; i += 12) {
        alignas(16) uint8_t idx[16];
        for (int g = 0; g < 4; ++g) {
            const uint8_t b0 = data[i + g*3 + 0];
            const uint8_t b1 = data[i + g*3 + 1];
            const uint8_t b2 = data[i + g*3 + 2];
            idx[g*4+0] = (b0 >> 2) & 0x3F;
            idx[g*4+1] = static_cast<uint8_t>(((b0 & 3) << 4) | (b1 >> 4));
            idx[g*4+2] = static_cast<uint8_t>(((b1 & 0xF) << 2) | (b2 >> 6));
            idx[g*4+3] = b2 & 0x3F;
        }

        const uint8x16_t vidx    = vld1q_u8(idx);
        const uint8x16_t kMask16 = vdupq_n_u8(0x0F);
        const uint8x16_t kT0     = vdupq_n_u8(15);
        const uint8x16_t kT1     = vdupq_n_u8(31);
        const uint8x16_t kT2     = vdupq_n_u8(47);

        const uint8x16_t lo_idx  = vandq_u8(vidx, kMask16);
        const uint8x16_t in_q0   = vcleq_u8(vidx, kT0);
        const uint8x16_t in_q1   = vandq_u8(vcgtq_u8(vidx, kT0), vcleq_u8(vidx, kT1));
        const uint8x16_t in_q2   = vandq_u8(vcgtq_u8(vidx, kT1), vcleq_u8(vidx, kT2));
        const uint8x16_t in_q3   = vcgtq_u8(vidx, kT2);

        const uint8x16_t si1 = vsubq_u8(vidx, vdupq_n_u8(16));
        const uint8x16_t si2 = vsubq_u8(vidx, vdupq_n_u8(32));
        const uint8x16_t si3 = vsubq_u8(vidx, vdupq_n_u8(48));

        const uint8x16_t r0 = vandq_u8(vqtbl1q_u8(vtbl0, lo_idx),                    in_q0);
        const uint8x16_t r1 = vandq_u8(vqtbl1q_u8(vtbl1, vandq_u8(si1, kMask16)),   in_q1);
        const uint8x16_t r2 = vandq_u8(vqtbl1q_u8(vtbl2, vandq_u8(si2, kMask16)),   in_q2);
        const uint8x16_t r3 = vandq_u8(vqtbl1q_u8(vtbl3, vandq_u8(si3, kMask16)),   in_q3);

        const uint8x16_t result = vorrq_u8(vorrq_u8(r0, r1), vorrq_u8(r2, r3));
        const size_t old_sz = out.size();
        out.resize(old_sz + 16);
        vst1q_u8(reinterpret_cast<uint8_t*>(out.data() + old_sz), result);
    }
    // Scalar tail
    for (; i < len; i += 3) {
        uint32_t b  = static_cast<uint8_t>(data[i]) << 16;
        if (i + 1 < len) b |= static_cast<uint8_t>(data[i+1]) << 8;
        if (i + 2 < len) b |= static_cast<uint8_t>(data[i+2]);
        out += kB64Table[(b >> 18) & 0x3F];
        out += kB64Table[(b >> 12) & 0x3F];
        if (i + 1 < len) out += kB64Table[(b >> 6) & 0x3F];
        if (i + 2 < len) out += kB64Table[(b     ) & 0x3F];
    }
    return out;
}

static void test_base64url() {
    println("\n[5] Base64url encoding (NEON vqtbl1q_u8)");

    auto run = [](std::span<const uint8_t> data, std::string_view label) {
        std::string s = base64url_scalar(data.data(), data.size());
        std::string n = base64url_neon  (data.data(), data.size());
        check(s == n, label);
    };

    // Known RFC 4648 vectors (adapted to no-padding base64url)
    {
        run(std::span<const uint8_t>{}, "empty input");
        const uint8_t d1[] = {0x4D};
        run(d1, "1 byte: 'M' → 'TQ'");
        const uint8_t d2[] = {0x4D, 0x61};
        run(d2, "2 bytes: 'Ma' → 'TWE'");
        const uint8_t d3[] = {0x4D, 0x61, 0x6E};
        run(d3, "3 bytes: 'Man' → 'TWFu'");
    }

    // All-zero input, various lengths
    for (size_t len : {0u, 3u, 6u, 12u, 13u, 24u, 25u}) {
        std::vector<uint8_t> v(len, 0x00);
        run(v, std::string("all-zero len=") + std::to_string(len));
    }

    // All-0xFF input (exercises upper table entries, q3)
    for (size_t len : {3u, 12u, 15u}) {
        std::vector<uint8_t> v(len, 0xFF);
        run(v, std::string("all-0xFF len=") + std::to_string(len));
    }

    // Sequential bytes 0..N (exercises all alphabet ranges)
    {
        std::vector<uint8_t> v(48);
        for (size_t i = 0; i < 48; ++i) v[i] = static_cast<uint8_t>(i);
        run(v, "sequential 0..47 (covers all 4 vtbl sub-tables)");
    }

    // 12 bytes exactly (one NEON iteration, zero tail)
    {
        std::vector<uint8_t> v = {0x00,0x10,0x83,0x10,0x51,0x87,0x20,0x92,0x8B,0x30,0xD3,0x8F};
        run(v, "12 bytes: exactly one NEON pass, no tail");
    }

    // Known-value cross-check: "Hello, World!" as bytes
    {
        const char src[] = "Hello, World!";
        const uint8_t* p = reinterpret_cast<const uint8_t*>(src);
        std::string s = base64url_scalar(p, strlen(src));
        std::string n = base64url_neon  (p, strlen(src));
        check(s == n, "'Hello, World!' encoding matches scalar");
        check(n == "SGVsbG8sIFdvcmxkIQ", "'Hello, World!' value correct (no padding)");
    }
}

// ─── 6. GF(2^8) erasure coding multiply-accumulate ───────────────────────────

// Scalar GF(2^8) multiply (Russian peasant, constant-time)
static constexpr uint8_t kGFPoly = 0x1d;

static constexpr uint8_t gf_mul(uint8_t a, uint8_t b) noexcept {
    uint8_t result = 0;
    for (int i = 0; i < 8; ++i) {
        result ^= static_cast<uint8_t>(-(b & 1) & a);
        uint8_t hbit = a >> 7;
        a = static_cast<uint8_t>(a << 1);
        a ^= static_cast<uint8_t>(-hbit & kGFPoly);
        b >>= 1;
    }
    return result;
}

// Build log/alog tables for fast_mul
static std::array<uint8_t,256> make_log_tbl() noexcept {
    std::array<uint8_t,256> t{};
    uint8_t x = 1;
    for (int i = 0; i < 255; ++i) { t[x] = static_cast<uint8_t>(i); x = gf_mul(x, 2); }
    return t;
}
static std::array<uint8_t,256> make_alog_tbl() noexcept {
    std::array<uint8_t,256> t{};
    uint8_t x = 1;
    for (int i = 0; i < 256; ++i) { t[i] = x; x = gf_mul(x, 2); }
    return t;
}
static const auto kLog  = make_log_tbl();
static const auto kAlog = make_alog_tbl();
static uint8_t fast_mul(uint8_t a, uint8_t b) noexcept {
    if (a == 0 || b == 0) return 0;
    return kAlog[(static_cast<int>(kLog[a]) + kLog[b]) % 255];
}

// Scalar muladd: out[i] ^= gf_mul(coeff, in[i])
static void gf_muladd_scalar(uint8_t coeff,
                              std::span<const uint8_t> in,
                              std::span<uint8_t>       out) noexcept {
    const size_t n = in.size();
    for (size_t i = 0; i < n; ++i)
        out[i] ^= fast_mul(coeff, in[i]);
}

// NEON muladd (from include/qbuem/buf/simd_erasure.hpp)
static void gf_muladd_neon(uint8_t coeff,
                            std::span<const uint8_t> in,
                            std::span<uint8_t>       out) noexcept {
    const size_t n = in.size();
    alignas(16) uint8_t tbl_lo[16], tbl_hi[16];
    for (int i = 0; i < 16; ++i) tbl_lo[i] = fast_mul(coeff, static_cast<uint8_t>(i));
    for (int i = 0; i < 16; ++i) tbl_hi[i] = fast_mul(coeff, static_cast<uint8_t>(i << 4));

    const uint8x16_t vtbl_lo = vld1q_u8(tbl_lo);
    const uint8x16_t vtbl_hi = vld1q_u8(tbl_hi);
    const uint8x16_t mask_lo = vdupq_n_u8(0x0F);

    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        const uint8x16_t src = vld1q_u8(in.data() + i);
        const uint8x16_t dst = vld1q_u8(out.data() + i);
        const uint8x16_t lo_nibble = vandq_u8(src, mask_lo);
        const uint8x16_t hi_nibble = vshrq_n_u8(src, 4);
        const uint8x16_t res = veorq_u8(
            vqtbl1q_u8(vtbl_lo, lo_nibble),
            vqtbl1q_u8(vtbl_hi, hi_nibble));
        vst1q_u8(out.data() + i, veorq_u8(dst, res));
    }
    for (; i < n; ++i)
        out[i] ^= fast_mul(coeff, in[i]);
}

static void test_gf_muladd() {
    println("\n[6] GF(2^8) erasure coding multiply-accumulate (vqtbl1q_u8)");

    auto run = [](uint8_t coeff, size_t len, std::string_view label) {
        std::vector<uint8_t> in(len), out_scalar(len, 0), out_neon(len, 0);
        for (size_t i = 0; i < len; ++i) in[i] = static_cast<uint8_t>(i * 11 + 7);

        gf_muladd_scalar(coeff, in, out_scalar);
        gf_muladd_neon  (coeff, in, out_neon);
        check(out_scalar == out_neon, label);
    };

    // Various coefficients
    run(0x00, 32, "coeff=0x00 (multiply by zero)");
    run(0x01, 32, "coeff=0x01 (identity)");
    run(0x02, 32, "coeff=0x02 (alpha)");
    run(0xFF, 32, "coeff=0xFF (max coefficient)");
    run(0x36, 32, "coeff=0x36 (arbitrary)");
    run(0xE7, 32, "coeff=0xE7 (arbitrary)");

    // Various lengths
    run(0xAB,  0,  "coeff=0xAB len=0: empty");
    run(0xAB,  1,  "coeff=0xAB len=1: single byte");
    run(0xAB, 15,  "coeff=0xAB len=15: one chunk - 1");
    run(0xAB, 16,  "coeff=0xAB len=16: exactly one chunk");
    run(0xAB, 17,  "coeff=0xAB len=17: one chunk + tail");
    run(0xAB, 64,  "coeff=0xAB len=64: four chunks");
    run(0xAB, 100, "coeff=0xAB len=100: non-multiple of 16");

    // RS property: muladd twice with same coeff → original out (XOR idempotence)
    {
        const uint8_t coeff = 0x5A;
        const size_t  n     = 48;
        std::vector<uint8_t> in(n), out(n, 0x42);
        for (size_t i = 0; i < n; ++i) in[i] = static_cast<uint8_t>(i * 13 + 1);
        std::vector<uint8_t> original_out = out;
        gf_muladd_neon(coeff, in, out);
        gf_muladd_neon(coeff, in, out);
        check(out == original_out, "RS property: double muladd recovers original out");
    }

    // GF multiplication table correctness: verify all 256 gf_mul values for coeff=0x02
    {
        const uint8_t coeff = 0x02;
        std::vector<uint8_t> in(256), out_neon(256, 0), out_scalar(256, 0);
        for (int i = 0; i < 256; ++i) in[i] = static_cast<uint8_t>(i);
        gf_muladd_scalar(coeff, in, out_scalar);
        gf_muladd_neon  (coeff, in, out_neon);
        check(out_scalar == out_neon, "all 256 GF(2^8) multiply values for coeff=2");
    }
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main() {
    println("qbuem-stack NEON validation on {}", []() -> std::string_view {
#if defined(__aarch64__)
        return "AArch64";
#else
        return "unknown arch";
#endif
    }());
    println("Compiler: " __VERSION__);
    println("NEON available: {}", []() -> std::string_view {
#if defined(__ARM_NEON)
        return "yes (__ARM_NEON defined)";
#else
        return "NO — tests will use scalar paths only";
#endif
    }());

    test_http_scanner();
    test_ws_masking();
    test_ct_compare();
    test_jwt_dots();
    test_base64url();
    test_gf_muladd();

    println("\n─────────────────────────────────────────");
    println("Results: {}/{} passed", g_passed, g_total);
    if (g_passed == g_total) {
        println("ALL NEON PATHS CORRECT");
        return 0;
    } else {
        println("FAILURES: {} test(s) failed", g_total - g_passed);
        return 1;
    }
}
