/**
 * @file bench/bench_crypto.cpp
 * @brief Cryptographic primitive throughput benchmarks.
 *
 * Measures the hot-path crypto the rest of the bench suite did not cover:
 * SHA-256, HMAC-SHA-256, ChaCha20-Poly1305 AEAD (seal/open), and AES-256-GCM
 * (only when AES-NI / ARM-AES is available). Reported as MB/s over the payload.
 */
#include "bench_common.hpp"

#include <qbuem/crypto/crypto.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include <qbuem/compat/print.hpp>

using namespace qbuem::crypto;

namespace {

// Payload sizes that bracket typical request bodies / records.
constexpr size_t kSizes[] = {64, 1024, 16384};

void bench_sha256() {
    bench::section("SHA-256 — one-shot hashing throughput");
    for (size_t sz : kSizes) {
        std::vector<uint8_t> data(sz, 0xABu);
        auto res = bench::run(
            "SHA-256 " + std::to_string(sz) + "B", 1000, 50000, [&] {
                auto d = sha256(std::span<const uint8_t>(data));
                bench::do_not_optimize(d);
            });
        res.print_throughput(sz);
    }
}

void bench_hmac_sha256() {
    bench::section("HMAC-SHA-256 — keyed MAC throughput");
    std::array<uint8_t, 32> key{};
    key.fill(0x0bu);
    for (size_t sz : kSizes) {
        std::vector<uint8_t> data(sz, 0xCDu);
        auto res = bench::run(
            "HMAC-SHA-256 " + std::to_string(sz) + "B", 1000, 50000, [&] {
                auto tag = hmac_sha256(std::span<const uint8_t>(key),
                                       std::span<const uint8_t>(data));
                bench::do_not_optimize(tag);
            });
        res.print_throughput(sz);
    }
}

void bench_chacha20_poly1305() {
    bench::section("ChaCha20-Poly1305 — AEAD seal/open throughput");
    AeadKey   key{};
    AeadNonce nonce{};
    key[0]   = 0x01u;
    nonce[0] = 0x02u;

    for (size_t sz : kSizes) {
        std::vector<uint8_t> pt(sz, 0xEFu);
        std::vector<uint8_t> ct(sz);
        std::vector<uint8_t> rec(sz);
        AeadTag tag{};

        auto sres = bench::run(
            "seal " + std::to_string(sz) + "B", 1000, 50000, [&] {
                auto r = chacha20_poly1305_seal(
                    key, nonce, {}, {pt.data(), pt.size()},
                    {ct.data(), ct.size()}, tag);
                bench::do_not_optimize(r);
            });
        sres.print_throughput(sz);

        auto ores = bench::run(
            "open " + std::to_string(sz) + "B", 1000, 50000, [&] {
                auto r = chacha20_poly1305_open(
                    key, nonce, {}, {ct.data(), ct.size()}, tag,
                    {rec.data(), rec.size()});
                bench::do_not_optimize(r);
            });
        ores.print_throughput(sz);
    }
}

void bench_aes_gcm() {
    bench::section("AES-256-GCM — AEAD throughput (hardware AES)");
    if (!has_aes_ni()) {
        std::println("  (skipped — no AES-NI / ARM-AES on this CPU)");
        return;
    }
    AesGcm256::KeyArray key{};
    key.fill(0x2bu);
    auto ctx = AesGcm256::create(key);
    if (!ctx) {
        std::println("  (skipped — AesGcm256::create failed)");
        return;
    }
    AesGcmNonce nonce{};
    nonce[0] = 0x03u;

    for (size_t sz : kSizes) {
        std::vector<uint8_t> pt(sz, 0x5au);
        std::vector<uint8_t> ct(sz);
        std::vector<uint8_t> rec(sz);
        AesGcmTag tag{};

        auto sres = bench::run(
            "seal " + std::to_string(sz) + "B", 1000, 50000, [&] {
                ctx->seal(nonce, {}, {pt.data(), pt.size()},
                          {ct.data(), ct.size()}, tag);
                bench::do_not_optimize(ct);
            });
        sres.print_throughput(sz);

        auto ores = bench::run(
            "open " + std::to_string(sz) + "B", 1000, 50000, [&] {
                auto r = ctx->open(nonce, {}, {ct.data(), ct.size()}, tag,
                                   {rec.data(), rec.size()});
                bench::do_not_optimize(r);
            });
        ores.print_throughput(sz);
    }
}

} // namespace

int main() {
    std::println();
    std::println("══════════════════════════════════════════════════════════════");
    std::println("  qbuem-stack — Cryptography Performance Benchmark");
    std::println("══════════════════════════════════════════════════════════════");

    bench_sha256();
    bench_hmac_sha256();
    bench_chacha20_poly1305();
    bench_aes_gcm();

    std::println();
    std::println("══════════════════════════════════════════════════════════════");
    std::println("  Done");
    std::println("══════════════════════════════════════════════════════════════");
    std::println();
    return 0;
}
