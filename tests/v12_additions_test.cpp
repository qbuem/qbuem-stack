/**
 * @file tests/v12_additions_test.cpp
 * @brief Tests for v1.2 high-level additions: secretbox (easy AEAD + password
 *        hashing), JWT HS256 sign/verify + HmacJwtVerifier, and BufferedReader.
 *
 * The crypto/JWT helpers are synchronous and tested directly. BufferedReader is
 * a coroutine driven synchronously via run_sync() over an in-memory mock stream
 * (no reactor needed — the mock read completes synchronously).
 */

#include <qbuem/crypto/jwt.hpp>
#include <qbuem/crypto/random.hpp>
#include <qbuem/crypto/secretbox.hpp>
#include <qbuem/io/buffered_reader.hpp>
#include <qbuem/middleware/jwt_verifier.hpp>

#include <chrono>
#include <cstring>
#include <gtest/gtest.h>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace qbuem;

namespace {

std::span<const uint8_t> bytes_of(std::string_view s) {
  return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

// Drive a Task<T> to completion synchronously (mock I/O never suspends on a
// reactor). Free-function coroutine + by-value Task → no dangling-capture trap.
template <class T>
Task<void> drive(Task<T> t, std::optional<T>& out) {
  out = co_await std::move(t);
}
template <class T>
T run_sync(Task<T> t) {
  std::optional<T> out;
  auto d = drive<T>(std::move(t), out);
  (void)d.resume();
  return std::move(out.value());
}

// In-memory stream exposing read(span)->Task<Result<size_t>> (BufferedReader's
// only requirement). Hands out at most `chunk` bytes per read; 0 == EOF.
struct MockStream {
  std::string data;
  size_t pos = 0;
  size_t chunk = 8;
  Task<Result<size_t>> read(std::span<std::byte> buf) {
    size_t n = std::min({buf.size(), chunk, data.size() - pos});
    std::memcpy(buf.data(), data.data() + pos, n);
    pos += n;
    co_return n;
  }
};

} // namespace

// ─── secretbox: easy AEAD ─────────────────────────────────────────────────────

TEST(Secretbox, SealOpenRoundTrip) {
  auto key = crypto::random_bytes<32>().value();
  std::string msg = "the quick brown fox";
  auto sealed = crypto::seal_easy(key, bytes_of(msg));
  ASSERT_TRUE(sealed.has_value());
  EXPECT_EQ(sealed->size(), msg.size() + crypto::kSecretboxOverhead);

  auto opened = crypto::open_easy(key, *sealed);
  ASSERT_TRUE(opened.has_value());
  EXPECT_EQ(std::string(opened->begin(), opened->end()), msg);
}

TEST(Secretbox, DistinctNoncesPerSeal) {
  auto key = crypto::random_bytes<32>().value();
  auto a = crypto::seal_easy(key, bytes_of("x")).value();
  auto b = crypto::seal_easy(key, bytes_of("x")).value();
  // First 12 bytes are the random nonce — must differ (no reuse).
  EXPECT_NE(0, std::memcmp(a.data(), b.data(), 12));
}

TEST(Secretbox, TamperRejected) {
  auto key = crypto::random_bytes<32>().value();
  auto sealed = crypto::seal_easy(key, bytes_of("secret")).value();
  sealed[20] ^= 0x01; // flip a ciphertext/tag bit
  EXPECT_FALSE(crypto::open_easy(key, *&sealed).has_value());

  auto wrong = crypto::random_bytes<32>().value();
  auto sealed2 = crypto::seal_easy(key, bytes_of("secret")).value();
  EXPECT_FALSE(crypto::open_easy(wrong, sealed2).has_value()); // wrong key
}

TEST(Secretbox, AadMustMatch) {
  auto key = crypto::random_bytes<32>().value();
  auto sealed = crypto::seal_easy(key, bytes_of("m"), bytes_of("aad-1")).value();
  EXPECT_TRUE(crypto::open_easy(key, sealed, bytes_of("aad-1")).has_value());
  EXPECT_FALSE(crypto::open_easy(key, sealed, bytes_of("aad-2")).has_value());
}

TEST(Secretbox, PasswordHashVerify) {
  // Small iteration count keeps the test fast.
  auto phc = crypto::password_hash("hunter2", 1000);
  ASSERT_TRUE(phc.has_value());
  EXPECT_NE(phc->find("pbkdf2_sha256$1000$"), std::string::npos);
  EXPECT_TRUE(crypto::verify_password("hunter2", *phc));
  EXPECT_FALSE(crypto::verify_password("wrong", *phc));
  EXPECT_FALSE(crypto::verify_password("hunter2", "garbage"));
  // Two hashes of the same password differ (random salt).
  auto phc2 = crypto::password_hash("hunter2", 1000).value();
  EXPECT_NE(*phc, phc2);
  EXPECT_TRUE(crypto::verify_password("hunter2", phc2));
}

// ─── JWT HS256 ────────────────────────────────────────────────────────────────

TEST(Jwt, EncodeVerifyRoundTrip) {
  std::string key = "my-secret-key";
  std::string payload = R"({"sub":"user42","role":"admin"})";
  auto token = crypto::encode_jwt_hs256(payload, key);
  // header.payload.sig — three segments.
  EXPECT_EQ(std::count(token.begin(), token.end(), '.'), 2);

  auto verified = crypto::verify_jwt_hs256(token, key);
  ASSERT_TRUE(verified.has_value());
  EXPECT_EQ(*verified, payload);
}

TEST(Jwt, RejectsWrongKeyAndTamper) {
  auto token = crypto::encode_jwt_hs256(R"({"sub":"a"})", std::string_view("k1"));
  EXPECT_FALSE(crypto::verify_jwt_hs256(token, std::string_view("k2")).has_value());

  // Tamper with the payload segment → signature mismatch.
  auto bad = token;
  bad[bad.find('.') + 1] = (bad[bad.find('.') + 1] == 'A') ? 'B' : 'A';
  EXPECT_FALSE(crypto::verify_jwt_hs256(bad, std::string_view("k1")).has_value());
}

TEST(Jwt, RejectsAlgNone) {
  // Forge an alg=none token: base64url(header)+"."+base64url(payload)+"." (empty sig).
  auto b64 = [](std::string_view s) {
    return crypto::base64url_encode(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(s.data()),
                                 s.size()));
  };
  std::string forged = b64(R"({"alg":"none","typ":"JWT"})") + "." +
                       b64(R"({"sub":"admin"})") + ".";
  EXPECT_FALSE(crypto::verify_jwt_hs256(forged, std::string_view("k")).has_value());
}

TEST(Jwt, VerifierEnforcesExpiry) {
  middleware::HmacJwtVerifier verifier("secret");
  const long now = static_cast<long>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());

  // Valid (far future exp).
  auto good = crypto::encode_jwt_hs256(
      R"({"sub":"u1","exp":)" + std::to_string(now + 3600) + "}", "secret");
  auto c = verifier.verify(good);
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->subject, "u1");

  // Expired.
  auto expired = crypto::encode_jwt_hs256(
      R"({"sub":"u1","exp":)" + std::to_string(now - 10) + "}", "secret");
  EXPECT_FALSE(verifier.verify(expired).has_value());

  // not-before in the future.
  auto nbf = crypto::encode_jwt_hs256(
      R"({"sub":"u1","nbf":)" + std::to_string(now + 3600) + "}", "secret");
  EXPECT_FALSE(verifier.verify(nbf).has_value());
}

// ─── BufferedReader ───────────────────────────────────────────────────────────

TEST(BufferedReader, ReadLinesAcrossChunks) {
  MockStream s{.data = "alpha\nbeta\ngamma\n", .chunk = 3}; // 3 bytes/read
  BufferedReader<MockStream> reader{s};

  auto l1 = run_sync(reader.read_line());
  ASSERT_TRUE(l1.has_value());
  EXPECT_EQ(*l1, "alpha\n");
  auto l2 = run_sync(reader.read_line());
  ASSERT_TRUE(l2.has_value());
  EXPECT_EQ(*l2, "beta\n");
  auto l3 = run_sync(reader.read_line());
  ASSERT_TRUE(l3.has_value());
  EXPECT_EQ(*l3, "gamma\n");
  // EOF before next delimiter.
  auto l4 = run_sync(reader.read_line());
  EXPECT_FALSE(l4.has_value());
}

TEST(BufferedReader, ReadUntilCustomDelimAndMaxBytes) {
  MockStream s{.data = "a;b;c", .chunk = 2};
  BufferedReader<MockStream> reader{s};
  auto t1 = run_sync(reader.read_until(';'));
  ASSERT_TRUE(t1.has_value());
  EXPECT_EQ(*t1, "a;");

  // max_bytes exceeded before delimiter → message_size.
  MockStream big{.data = std::string(100, 'x'), .chunk = 16};
  BufferedReader<MockStream> r2{big};
  auto t2 = run_sync(r2.read_until('\n', /*max_bytes=*/16));
  EXPECT_FALSE(t2.has_value());
}
