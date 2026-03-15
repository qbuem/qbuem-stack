/**
 * @file security_v15_test.cpp
 * @brief v1.5.0 보안 컴포넌트 단위 테스트.
 *
 * 커버리지:
 * - SIMDJwtParser: 구조 파싱, 오류 케이스, 클레임 추출, 만료 검사
 * - Hardware Entropy: RDRAND/RDSEED 가용성 쿼리, hw_entropy_fill 기능
 * - crypto.hpp: constant_time_equal (기존), RDRAND 확장
 * - kTLS sendfile: 함수 선언 접근성 (플랫폼 폴백 포함)
 */

#include <qbuem/security/simd_jwt.hpp>
#include <qbuem/crypto.hpp>
#include <qbuem/io/ktls.hpp>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// SIMDJwtParser
// ─────────────────────────────────────────────────────────────────────────────

using namespace qbuem::security;

// RFC 7519 예시 토큰 (서명 검증 없음 — 구조 파싱만 테스트)
// header: {"alg":"HS256","typ":"JWT"}
// payload: {"sub":"1234567890","name":"John Doe","iat":1516239022}
static constexpr std::string_view kSampleJwt =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"
    "."
    "eyJzdWIiOiIxMjM0NTY3ODkwIiwibmFtZSI6IkpvaG4gRG9lIiwiaWF0IjoxNTE2MjM5MDIyfQ"
    "."
    "SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c";

TEST(SIMDJwtParser, ParseValidToken) {
    SIMDJwtParser parser;
    auto view = parser.parse(kSampleJwt);
    ASSERT_TRUE(view.has_value());
    EXPECT_FALSE(view->header.empty());
    EXPECT_FALSE(view->payload.empty());
    EXPECT_FALSE(view->signature.empty());
}

TEST(SIMDJwtParser, HeaderIsFirstPart) {
    SIMDJwtParser parser;
    auto view = parser.parse(kSampleJwt);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->header, "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9");
}

TEST(SIMDJwtParser, SignatureIsLastPart) {
    SIMDJwtParser parser;
    auto view = parser.parse(kSampleJwt);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->signature, "SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c");
}

TEST(SIMDJwtParser, SigningInputPointsIntoOriginal) {
    SIMDJwtParser parser;
    auto view = parser.parse(kSampleJwt);
    ASSERT_TRUE(view.has_value());
    auto si = view->signing_input(kSampleJwt);
    // header.payload (. 포함)
    size_t expected_len = view->header.size() + 1 + view->payload.size();
    EXPECT_EQ(si.size(), expected_len);
    EXPECT_EQ(si.data(), kSampleJwt.data()); // zero-copy: 원본 참조
}

TEST(SIMDJwtParser, ParseEmptyReturnsNullopt) {
    SIMDJwtParser parser;
    EXPECT_FALSE(parser.parse("").has_value());
}

TEST(SIMDJwtParser, ParseNoDotReturnsNullopt) {
    SIMDJwtParser parser;
    EXPECT_FALSE(parser.parse("nodotatall").has_value());
}

TEST(SIMDJwtParser, ParseOneDotReturnsNullopt) {
    SIMDJwtParser parser;
    EXPECT_FALSE(parser.parse("header.payload").has_value());
}

TEST(SIMDJwtParser, ParseInvalidBase64UrlHeaderReturnsNullopt) {
    SIMDJwtParser parser;
    // '+' 는 Base64url에서 유효하지 않은 문자
    EXPECT_FALSE(parser.parse("inv+lid.payload.sig").has_value());
}

TEST(SIMDJwtParser, ParseEmptyHeaderReturnsNullopt) {
    SIMDJwtParser parser;
    EXPECT_FALSE(parser.parse(".payload.sig").has_value());
}

TEST(SIMDJwtParser, ParseEmptyPayloadReturnsNullopt) {
    SIMDJwtParser parser;
    EXPECT_FALSE(parser.parse("header..sig").has_value());
}

TEST(SIMDJwtParser, ClaimIntIat) {
    SIMDJwtParser parser;
    auto view = parser.parse(kSampleJwt);
    ASSERT_TRUE(view.has_value());
    auto iat = view->claim_int("iat");
    ASSERT_TRUE(iat.has_value());
    EXPECT_EQ(*iat, 1516239022LL);
}

TEST(SIMDJwtParser, ClaimStringSub) {
    SIMDJwtParser parser;
    auto view = parser.parse(kSampleJwt);
    ASSERT_TRUE(view.has_value());
    auto sub = view->claim("sub");
    ASSERT_TRUE(sub.has_value());
    EXPECT_EQ(*sub, "1234567890");
}

TEST(SIMDJwtParser, ClaimMissingReturnsNullopt) {
    SIMDJwtParser parser;
    auto view = parser.parse(kSampleJwt);
    ASSERT_TRUE(view.has_value());
    EXPECT_FALSE(view->claim("exp").has_value());
}

TEST(SIMDJwtParser, IsExpiredNoExpClaim) {
    SIMDJwtParser parser;
    auto view = parser.parse(kSampleJwt);
    ASSERT_TRUE(view.has_value());
    // exp 클레임 없음 → 만료로 간주하지 않음
    EXPECT_FALSE(view->is_expired(9999999999LL));
}

// ─────────────────────────────────────────────────────────────────────────────
// Hardware Entropy
// ─────────────────────────────────────────────────────────────────────────────

TEST(HardwareEntropy, HasRdrandReturnsBool) {
    // 반환값 타입과 호출 가능 여부만 확인 (CPU 종류 무관)
    bool r = qbuem::has_rdrand();
    (void)r; // 값은 하드웨어에 따라 다름
    SUCCEED();
}

TEST(HardwareEntropy, HasRdseedReturnsBool) {
    bool r = qbuem::has_rdseed();
    (void)r;
    SUCCEED();
}

TEST(HardwareEntropy, HwEntropyFillProducesNonZeroBuffer) {
    // 32바이트 엔트로피를 채우고, 전부 0이 아닌지 확인
    std::array<uint8_t, 32> buf{};
    EXPECT_NO_THROW(qbuem::hw_entropy_fill(buf.data(), buf.size()));

    // 모두 0일 확률은 극히 낮음 (암호학적 무작위성 기본 검사)
    bool all_zero = true;
    for (auto b : buf) if (b != 0) { all_zero = false; break; }
    EXPECT_FALSE(all_zero);
}

TEST(HardwareEntropy, HwEntropyFill1Byte) {
    uint8_t byte = 0xAA;
    EXPECT_NO_THROW(qbuem::hw_entropy_fill(&byte, 1));
    // 1바이트 주입 성공 여부만 확인
    SUCCEED();
}

TEST(HardwareEntropy, HwEntropyFill64Bytes) {
    std::array<uint8_t, 64> buf{};
    EXPECT_NO_THROW(qbuem::hw_entropy_fill(buf.data(), buf.size()));
    SUCCEED();
}

TEST(HardwareEntropy, RdrandConsecutiveDiffer) {
    if (!qbuem::has_rdrand()) GTEST_SKIP() << "RDRAND not supported";
    uint64_t a = 0, b = 0;
    EXPECT_TRUE(qbuem::rdrand64(a));
    EXPECT_TRUE(qbuem::rdrand64(b));
    // 연속 두 값이 같을 확률은 2^-64 — 실질적으로 0
    EXPECT_NE(a, b);
}

TEST(HardwareEntropy, HwSeedFillProducesNonZero) {
    std::array<uint8_t, 32> buf{};
    EXPECT_NO_THROW(qbuem::hw_seed_fill(buf.data(), buf.size()));
    bool all_zero = true;
    for (auto b : buf) if (b != 0) { all_zero = false; break; }
    EXPECT_FALSE(all_zero);
}

// ─────────────────────────────────────────────────────────────────────────────
// kTLS sendfile API 접근성
// ─────────────────────────────────────────────────────────────────────────────

TEST(KtlsSendfile, FunctionAccessible) {
    // 실제 fd 없이 함수 존재 여부만 확인 (컴파일 테스트)
    // 비Linux에서는 errc::not_supported 반환
    off_t off = 0;
    auto r = qbuem::io::ktls_sendfile(-1, -1, off, 0);
    // Linux: 잘못된 fd → EBADF
    // 비Linux: not_supported
    EXPECT_FALSE(r.has_value()); // 어떤 경우든 에러
}

TEST(KtlsSendfile, AllVariantAccessible) {
    auto r = qbuem::io::ktls_sendfile_all(-1, -1);
    EXPECT_FALSE(r.has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// constant_time_equal (기존 — 회귀 테스트)
// ─────────────────────────────────────────────────────────────────────────────

TEST(ConstantTimeEqual, EqualStrings) {
    EXPECT_TRUE(qbuem::constant_time_equal("hello", "hello"));
}

TEST(ConstantTimeEqual, DifferentStrings) {
    EXPECT_FALSE(qbuem::constant_time_equal("hello", "world"));
}

TEST(ConstantTimeEqual, DifferentLengths) {
    EXPECT_FALSE(qbuem::constant_time_equal("short", "longer_string"));
}

TEST(ConstantTimeEqual, EmptyStrings) {
    EXPECT_TRUE(qbuem::constant_time_equal("", ""));
}
