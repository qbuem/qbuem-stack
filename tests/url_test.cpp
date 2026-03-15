/**
 * @file tests/url_test.cpp
 * @brief Unit tests for qbuem URL utility functions (url_encode / url_decode).
 */

#include <gtest/gtest.h>
#include <qbuem/url.hpp>

using namespace qbuem;

// ─── url_decode ───────────────────────────────────────────────────────────────

TEST(UrlDecodeTest, PlainStringUnchanged) {
    EXPECT_EQ(url_decode("hello"), "hello");
}

TEST(UrlDecodeTest, PlusDecodedAsSpace) {
    EXPECT_EQ(url_decode("hello+world"), "hello world");
}

TEST(UrlDecodeTest, PercentEncodedSpace) {
    EXPECT_EQ(url_decode("hello%20world"), "hello world");
}

TEST(UrlDecodeTest, PercentEncodedExclamation) {
    EXPECT_EQ(url_decode("hello%21"), "hello!");
}

TEST(UrlDecodeTest, FullQueryString) {
    EXPECT_EQ(url_decode("hello%20world%21"), "hello world!");
}

TEST(UrlDecodeTest, InvalidPercentSequencePassedThrough) {
    // %ZZ is invalid — should pass through unchanged
    EXPECT_EQ(url_decode("%ZZ"), "%ZZ");
}

TEST(UrlDecodeTest, TruncatedPercentSequencePassedThrough) {
    // % at end of string — pass through
    EXPECT_EQ(url_decode("abc%"), "abc%");
}

TEST(UrlDecodeTest, UppercaseHexDigits) {
    EXPECT_EQ(url_decode("%2F"), "/");
    EXPECT_EQ(url_decode("%41"), "A");
}

TEST(UrlDecodeTest, LowercaseHexDigits) {
    EXPECT_EQ(url_decode("%2f"), "/");
    EXPECT_EQ(url_decode("%41"), "A");
}

TEST(UrlDecodeTest, EmptyString) {
    EXPECT_EQ(url_decode(""), "");
}

// ─── url_encode ───────────────────────────────────────────────────────────────

TEST(UrlEncodeTest, AlphanumericUnchanged) {
    EXPECT_EQ(url_encode("hello123"), "hello123");
}

TEST(UrlEncodeTest, SpaceEncodedAsPercent20) {
    EXPECT_EQ(url_encode("hello world"), "hello%20world");
}

TEST(UrlEncodeTest, ExclamationEncoded) {
    EXPECT_EQ(url_encode("hello!"), "hello%21");
}

TEST(UrlEncodeTest, UnreservedCharsUnchanged) {
    // RFC 3986 §2.3 unreserved: A-Z a-z 0-9 - _ . ~
    EXPECT_EQ(url_encode("aZ-_.~"), "aZ-_.~");
}

TEST(UrlEncodeTest, SlashEncoded) {
    EXPECT_EQ(url_encode("/"), "%2F");
}

TEST(UrlEncodeTest, EmptyString) {
    EXPECT_EQ(url_encode(""), "");
}

TEST(UrlEncodeTest, RoundTrip) {
    const std::string original = "hello world! @#$%^&*()";
    EXPECT_EQ(url_decode(url_encode(original)), original);
}

TEST(UrlEncodeTest, UppercaseHexOutput) {
    // url_encode uses uppercase hex (A-F)
    std::string encoded = url_encode(" ");
    EXPECT_EQ(encoded, "%20");
}
