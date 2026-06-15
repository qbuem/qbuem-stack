/**
 * @file tests/middleware_extra_test.cpp
 * @brief Unit tests for the content_type and body_encoder middleware.
 *
 * Covers:
 *   - require_content_type() / require_json():
 *       correct Content-Type passes; wrong/missing is rejected (415);
 *       non-enforced methods pass through; non-JSON variant (415);
 *       require_content_type with a custom method set.
 *   - body_encoder (compress_response):
 *       a compressible body over min_size with a matching Accept-Encoding
 *       gets encoded (Content-Encoding + Vary headers set, body replaced);
 *       below threshold, wrong content-type, missing Accept-Encoding, or an
 *       already-encoded response is left unchanged; encoder failure leaves the
 *       original body intact.
 *
 * Style mirrors tests/http_middleware_test.cpp (gtest TEST() macros, the
 * TestReq helper that owns a heap-stable receive buffer so the zero-copy
 * string_views inside Request stay valid).
 */

#include <gtest/gtest.h>
#include <qbuem/http/parser.hpp>
#include <qbuem/http/request.hpp>
#include <qbuem/http/response.hpp>
#include <qbuem/middleware/body_encoder.hpp>
#include <qbuem/middleware/content_type.hpp>

#include <memory>
#include <string>
#include <string_view>

using namespace qbuem;
using namespace qbuem::middleware;

// ─── Helpers ──────────────────────────────────────────────────────────────────
//
// Request stores string_views into a receive buffer (zero-copy design).
// The buffer MUST outlive the Request. TestReq owns the buffer via unique_ptr
// so the heap address is stable even if TestReq is moved — string_views in
// req remain valid for the lifetime of the TestReq object.

struct TestReq {
    std::unique_ptr<std::string> raw_buf; // heap-stable; string_views point here
    Request req;
};

// Build a request with an arbitrary method, optional Content-Type, optional
// Accept-Encoding, and an optional body (a correct Content-Length is emitted
// so the parser extracts the body via the fixed-length path).
static TestReq make_req(const char* method, const char* path,
                        const char* content_type = nullptr,
                        const char* accept_encoding = nullptr,
                        const char* body = nullptr) {
    TestReq tr;
    std::string raw =
        std::string(method) + " " + path + " HTTP/1.1\r\nHost: localhost\r\n";
    if (content_type)    raw += std::string("Content-Type: ")    + content_type    + "\r\n";
    if (accept_encoding) raw += std::string("Accept-Encoding: ") + accept_encoding + "\r\n";
    std::string body_str = body ? std::string(body) : std::string();
    if (body) raw += "Content-Length: " + std::to_string(body_str.size()) + "\r\n";
    raw += "\r\n";
    raw += body_str;

    tr.raw_buf = std::make_unique<std::string>(std::move(raw));
    HttpParser parser;
    parser.parse(*tr.raw_buf, tr.req);
    return tr;
}

// ─── Test encoders (zero external deps) ─────────────────────────────────────────

// A deterministic, dependency-free "encoder": reverses the input bytes.
// encode() always succeeds, so it isolates the policy logic of compress_response
// from any real compression algorithm.
class ReverseEncoder final : public IBodyEncoder {
public:
    bool encode(std::string_view src, std::string& dst) noexcept override {
        dst.assign(src.rbegin(), src.rend());
        ++calls_;
        return true;
    }
    std::string_view encoding_name() const noexcept override { return "rev"; }
    std::string_view accept_token()  const noexcept override { return "rev"; }
    int calls() const noexcept { return calls_; }
private:
    int calls_ = 0;
};

// An encoder whose encode() always fails — used to verify the original body is
// left untouched on failure.
class FailingEncoder final : public IBodyEncoder {
public:
    bool encode(std::string_view, std::string&) noexcept override {
        ++calls_;
        return false;
    }
    std::string_view encoding_name() const noexcept override { return "fail"; }
    std::string_view accept_token()  const noexcept override { return "fail"; }
    int calls() const noexcept { return calls_; }
private:
    int calls_ = 0;
};

// ════════════════════════════════════════════════════════════════════════════
//  require_content_type() / require_json()
// ════════════════════════════════════════════════════════════════════════════

TEST(RequireJsonTest, CorrectContentTypePasses) {
    auto mw = require_json();
    auto tr = make_req("POST", "/api/data", "application/json");
    Response res;
    bool cont = mw(tr.req, res);
    EXPECT_TRUE(cont);                 // chain continues
    EXPECT_EQ(res.status_code(), 200); // status untouched (default)
}

TEST(RequireJsonTest, ContentTypeWithCharsetParameterPasses) {
    // The check is a substring match, so "application/json; charset=utf-8" matches.
    auto mw = require_json();
    auto tr = make_req("POST", "/api/data", "application/json; charset=utf-8");
    Response res;
    bool cont = mw(tr.req, res);
    EXPECT_TRUE(cont);
}

TEST(RequireJsonTest, WrongContentTypeRejectedWith415) {
    auto mw = require_json();
    auto tr = make_req("POST", "/api/data", "text/plain");
    Response res;
    bool cont = mw(tr.req, res);
    EXPECT_FALSE(cont);                 // chain halted
    EXPECT_EQ(res.status_code(), 415);  // Unsupported Media Type
    EXPECT_NE(res.get_body().find("application/json"), std::string_view::npos);
}

TEST(RequireJsonTest, MissingContentTypeRejectedWith415) {
    auto mw = require_json();
    auto tr = make_req("PUT", "/api/data"); // no Content-Type header
    Response res;
    bool cont = mw(tr.req, res);
    EXPECT_FALSE(cont);
    EXPECT_EQ(res.status_code(), 415);
}

TEST(RequireJsonTest, PatchIsEnforced) {
    // PATCH is one of the default enforced methods.
    auto mw = require_json();
    auto tr = make_req("PATCH", "/api/data", "text/plain");
    Response res;
    bool cont = mw(tr.req, res);
    EXPECT_FALSE(cont);
    EXPECT_EQ(res.status_code(), 415);
}

TEST(RequireJsonTest, GetMethodNotEnforcedPassesEvenWithoutContentType) {
    // GET is not in the default {POST, PUT, PATCH} set → always passes through.
    auto mw = require_json();
    auto tr = make_req("GET", "/api/data"); // no Content-Type at all
    Response res;
    bool cont = mw(tr.req, res);
    EXPECT_TRUE(cont);
    EXPECT_EQ(res.status_code(), 200);
}

TEST(RequireJsonTest, GetMethodWithWrongContentTypeStillPasses) {
    // Even a wrong Content-Type is irrelevant for a non-enforced method.
    auto mw = require_json();
    auto tr = make_req("GET", "/api/data", "text/plain");
    Response res;
    bool cont = mw(tr.req, res);
    EXPECT_TRUE(cont);
    EXPECT_EQ(res.status_code(), 200);
}

TEST(RequireContentTypeTest, CustomTypeMatches) {
    auto mw = require_content_type("multipart/form-data");
    auto tr = make_req("POST", "/upload",
                       "multipart/form-data; boundary=----abc");
    Response res;
    bool cont = mw(tr.req, res);
    EXPECT_TRUE(cont);
    EXPECT_EQ(res.status_code(), 200);
}

TEST(RequireContentTypeTest, CustomTypeMismatchRejectedWith415) {
    auto mw = require_content_type("multipart/form-data");
    auto tr = make_req("POST", "/upload", "application/json");
    Response res;
    bool cont = mw(tr.req, res);
    EXPECT_FALSE(cont);
    EXPECT_EQ(res.status_code(), 415);
    EXPECT_NE(res.get_body().find("multipart/form-data"),
              std::string_view::npos);
}

TEST(RequireContentTypeTest, CustomMethodSetOnlyEnforcesListedMethods) {
    // Enforce only on POST; a PUT request with a wrong type must pass through.
    auto mw = require_content_type("application/json", {Method::Post});

    auto put = make_req("PUT", "/api/data", "text/plain");
    Response put_res;
    EXPECT_TRUE(mw(put.req, put_res));        // PUT not enforced → passes
    EXPECT_EQ(put_res.status_code(), 200);

    auto post = make_req("POST", "/api/data", "text/plain");
    Response post_res;
    EXPECT_FALSE(mw(post.req, post_res));     // POST enforced → rejected
    EXPECT_EQ(post_res.status_code(), 415);
}

// ════════════════════════════════════════════════════════════════════════════
//  body_encoder — compress_response policy
// ════════════════════════════════════════════════════════════════════════════

// A body comfortably above the default 256 B min_size and a compressible type.
static std::string big_json_body() {
    // 400 bytes of repeated JSON-ish text.
    std::string s;
    while (s.size() < 400) s += R"({"k":"value","n":12345},)";
    return s;
}

TEST(CompressResponseTest, CompressibleBodyOverThresholdIsEncoded) {
    ReverseEncoder enc;
    auto tr = make_req("GET", "/api/data", nullptr, "rev");
    const std::string original = big_json_body();

    Response res;
    res.status(200).header("Content-Type", "application/json").body(original);

    compress_response(enc, tr.req, res); // default min_size = 256

    EXPECT_EQ(enc.calls(), 1);                          // encoder was invoked
    EXPECT_EQ(res.get_header("Content-Encoding"), "rev");
    EXPECT_EQ(res.get_header("Vary"), "Accept-Encoding");
    // Reverse encoder → body must equal the reversed original, i.e. changed.
    std::string reversed(original.rbegin(), original.rend());
    EXPECT_EQ(res.get_body(), reversed);
    EXPECT_NE(res.get_body(), original);
}

TEST(CompressResponseTest, BodyBelowThresholdLeftUnchanged) {
    ReverseEncoder enc;
    auto tr = make_req("GET", "/api/data", nullptr, "rev");
    const std::string small = "{\"x\":1}"; // 7 bytes < 256

    Response res;
    res.status(200).header("Content-Type", "application/json").body(small);

    compress_response(enc, tr.req, res);

    EXPECT_EQ(enc.calls(), 0);                       // encoder never called
    EXPECT_TRUE(res.get_header("Content-Encoding").empty());
    EXPECT_EQ(res.get_body(), small);               // unchanged
}

TEST(CompressResponseTest, CustomMinSizeBoundaryRespected) {
    ReverseEncoder enc;
    auto tr = make_req("GET", "/api/data", nullptr, "rev");
    const std::string body(300, 'a');               // 300 bytes

    Response res;
    res.status(200).header("Content-Type", "text/plain").body(body);

    // min_size = 512 → 300-byte body is below threshold, must stay unchanged.
    compress_response(enc, tr.req, res, /*min_size=*/512);

    EXPECT_EQ(enc.calls(), 0);
    EXPECT_EQ(res.get_body(), body);
    EXPECT_TRUE(res.get_header("Content-Encoding").empty());
}

TEST(CompressResponseTest, NonCompressibleContentTypeLeftUnchanged) {
    ReverseEncoder enc;
    auto tr = make_req("GET", "/img", nullptr, "rev");
    const std::string body(500, '\x01');            // binary, > 256

    Response res;
    res.status(200).header("Content-Type", "image/png").body(body);

    compress_response(enc, tr.req, res);

    EXPECT_EQ(enc.calls(), 0);                       // image/png not compressible
    EXPECT_EQ(res.get_body(), body);
    EXPECT_TRUE(res.get_header("Content-Encoding").empty());
}

TEST(CompressResponseTest, MissingAcceptEncodingLeftUnchanged) {
    ReverseEncoder enc;
    auto tr = make_req("GET", "/api/data"); // no Accept-Encoding header
    const std::string body = big_json_body();

    Response res;
    res.status(200).header("Content-Type", "application/json").body(body);

    compress_response(enc, tr.req, res);

    EXPECT_EQ(enc.calls(), 0);                       // client did not advertise
    EXPECT_EQ(res.get_body(), body);
    EXPECT_TRUE(res.get_header("Content-Encoding").empty());
}

TEST(CompressResponseTest, NonMatchingAcceptEncodingLeftUnchanged) {
    ReverseEncoder enc;                               // accept_token() == "rev"
    auto tr = make_req("GET", "/api/data", nullptr, "gzip, br"); // no "rev"
    const std::string body = big_json_body();

    Response res;
    res.status(200).header("Content-Type", "application/json").body(body);

    compress_response(enc, tr.req, res);

    EXPECT_EQ(enc.calls(), 0);
    EXPECT_EQ(res.get_body(), body);
    EXPECT_TRUE(res.get_header("Content-Encoding").empty());
}

TEST(CompressResponseTest, AlreadyEncodedResponseLeftUnchanged) {
    ReverseEncoder enc;
    auto tr = make_req("GET", "/api/data", nullptr, "rev");
    const std::string body = big_json_body();

    Response res;
    res.status(200)
        .header("Content-Type", "application/json")
        .header("Content-Encoding", "br") // already encoded by something else
        .body(body);

    compress_response(enc, tr.req, res);

    EXPECT_EQ(enc.calls(), 0);                       // must not double-encode
    EXPECT_EQ(res.get_header("Content-Encoding"), "br"); // preserved
    EXPECT_EQ(res.get_body(), body);                 // body untouched
}

TEST(CompressResponseTest, EncoderFailureLeavesOriginalBody) {
    FailingEncoder enc;
    auto tr = make_req("GET", "/api/data", nullptr, "fail");
    const std::string body = big_json_body();

    Response res;
    res.status(200).header("Content-Type", "application/json").body(body);

    compress_response(enc, tr.req, res);

    EXPECT_EQ(enc.calls(), 1);                       // attempted once
    EXPECT_EQ(res.get_body(), body);                 // unchanged on failure
    EXPECT_TRUE(res.get_header("Content-Encoding").empty()); // no header set
}

TEST(CompressResponseTest, AcceptEncodingMatchesAmongMultipleTokens) {
    ReverseEncoder enc;
    // "rev" appears in a comma-separated Accept-Encoding list.
    auto tr = make_req("GET", "/api/data", nullptr, "gzip, rev, br");
    const std::string body = big_json_body();

    Response res;
    res.status(200).header("Content-Type", "text/html").body(body);

    compress_response(enc, tr.req, res);

    EXPECT_EQ(enc.calls(), 1);
    EXPECT_EQ(res.get_header("Content-Encoding"), "rev");
    std::string reversed(body.rbegin(), body.rend());
    EXPECT_EQ(res.get_body(), reversed);
}
