#pragma once

/**
 * @file qbuem/server/http2_handler.hpp
 * @brief HTTP/2 connection handler — frame processing, HPACK, stream state management
 * @defgroup qbuem_http2_handler HTTP/2 Handler
 * @ingroup qbuem_server
 *
 * This header implements the core connection management of the HTTP/2 protocol as a header-only library.
 *
 * ### Key Features
 * - **HTTP/2 frame parsing/serialization**: DATA, HEADERS, SETTINGS, PING, GOAWAY, etc.
 * - **HPACK decoder/encoder**: Header compression based on Static Table (RFC 7541)
 * - **Stream state machine**: IDLE → OPEN → HALF_CLOSED → CLOSED transition management
 * - **Connection preface**: Used after TLS handshake when "h2" ALPN negotiation succeeds
 * - **Coroutine-based async processing**: Uses C++20 Task<> and co_return
 *
 * ### Limitations (minimal implementation)
 * - HPACK dynamic table not supported (only static table 62 entries + literal encoding)
 * - PRIORITY and PUSH_PROMISE frames are silently ignored on receipt
 * - Flow Control not implemented
 *
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/core/task.hpp>
#include <qbuem/http/request.hpp>
#include <qbuem/http/response.hpp>
#include <qbuem/pipeline/async_channel.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace qbuem {

// ─── Http2FrameType ───────────────────────────────────────────────────────────

/**
 * @brief HTTP/2 frame type enumeration (RFC 7540 section 6).
 *
 * Each value corresponds to the `Type` field (1 byte) of an HTTP/2 frame header.
 */
enum class Http2FrameType : uint8_t {
    DATA          = 0x0, ///< Stream data delivery frame
    HEADERS       = 0x1, ///< Header block + stream priority frame
    PRIORITY      = 0x2, ///< Stream priority assignment frame
    RST_STREAM    = 0x3, ///< Stream forced termination frame
    SETTINGS      = 0x4, ///< Connection settings parameter exchange frame
    PUSH_PROMISE  = 0x5, ///< Server push announcement frame
    PING          = 0x6, ///< Connection liveness check and RTT measurement frame
    GOAWAY        = 0x7, ///< Connection termination notification frame
    WINDOW_UPDATE = 0x8, ///< Flow control window size update frame
    CONTINUATION  = 0x9, ///< HEADERS/PUSH_PROMISE continuation block frame
};

// ─── Http2Frame ───────────────────────────────────────────────────────────────

/**
 * @brief Structure representing a single HTTP/2 frame.
 *
 * Reflects the frame format defined in RFC 7540 section 4.1.
 *
 * ```
 * +-----------------------------------------------+
 * |                 Length (24)                   |
 * +---------------+---------------+---------------+
 * |   Type (8)    |   Flags (8)   |
 * +-+-------------+---------------+-------------------------------+
 * |R|                 Stream Identifier (31)                      |
 * +=+=============================================================+
 * |                   Frame Payload (0...)                        |
 * +---------------------------------------------------------------+
 * ```
 *
 * @note The `length` field is 24-bit but stored as uint32_t (upper 8 bits ignored).
 * @note `stream_id` is 31-bit (MSB is reserved bit, always 0).
 */
struct Http2Frame {
    uint32_t              length{0};    ///< Payload byte count (24-bit valid value)
    Http2FrameType        type{Http2FrameType::DATA}; ///< Frame type
    uint8_t               flags{0};     ///< Type-specific flag bitfield
    uint32_t              stream_id{0}; ///< Stream identifier (31-bit, MSB reserved)
    std::vector<uint8_t>  payload;      ///< Frame payload bytes
};

// ─── HPACK static table flag constants ──────────────────────────────────────

/// @brief END_STREAM flag for HEADERS frames (bit 0)
static constexpr uint8_t HTTP2_FLAG_END_STREAM  = 0x1;
/// @brief END_HEADERS flag for HEADERS/CONTINUATION frames (bit 2)
static constexpr uint8_t HTTP2_FLAG_END_HEADERS = 0x4;
/// @brief ACK flag for SETTINGS frames (bit 0)
static constexpr uint8_t HTTP2_FLAG_ACK         = 0x1;
/// @brief PADDED flag for DATA frames (bit 3)
static constexpr uint8_t HTTP2_FLAG_PADDED      = 0x8;
/// @brief PRIORITY flag for HEADERS frames (bit 5)
static constexpr uint8_t HTTP2_FLAG_PRIORITY    = 0x20;

// ─── HpackDecoder ─────────────────────────────────────────────────────────────

/**
 * @brief HPACK header block decoder (RFC 7541).
 *
 * Minimal implementation supporting only the Static Table (indices 1~61).
 * Dynamic Table is not supported.
 *
 * ### Supported representation formats
 * - **Indexed Header Field** (RFC 7541 section 6.1): bit pattern `1xxxxxxx`
 * - **Literal Header Field — Without Indexing** (RFC 7541 section 6.2.2):
 *   - Name referenced by index: bit pattern `0000xxxx` (upper 4 bits 0)
 *   - Name as literal string: index == 0
 * - **Literal Header Field — Incremental Indexing** (RFC 7541 section 6.2.1):
 *   bit pattern `01xxxxxx` (partially supported; dynamic table updates are ignored)
 *
 * @warning Huffman encoding is not supported. Literal strings are processed as raw bytes only.
 */
class HpackDecoder {
public:
    /**
     * @brief Decodes a header block fragment and returns a header map.
     *
     * @param data HPACK-encoded header block fragment.
     * @returns Map of decoded header name-value pairs.
     *          If multiple headers share the same name, only the last value is preserved.
     */
    std::unordered_map<std::string, std::string> decode(std::span<const uint8_t> data) {
        std::unordered_map<std::string, std::string> headers;
        size_t pos = 0;
        const size_t len = data.size();

        while (pos < len) {
            uint8_t first = data[pos];

            if (first & 0x80) {
                // ── 인덱스 헤더 필드 표현 (RFC 7541 섹션 6.1) ──────────────
                // 비트 패턴: 1xxxxxxx
                // 정수 인코딩: 7비트 접두사
                uint32_t index = decode_integer(data, pos, 7);
                if (index > 0 && index < 62) {
                    const auto& [name, value] = STATIC_TABLE[index];
                    headers[std::string(name)] = std::string(value);
                }
                // index == 0은 에러, 62 이상은 동적 테이블(미지원) → 무시
            } else if ((first & 0xC0) == 0x40) {
                // ── 리터럴 헤더 필드 — 증분 인덱싱 (RFC 7541 섹션 6.2.1) ──
                // 비트 패턴: 01xxxxxx
                // 6비트 접두사로 이름 인덱스 디코딩
                uint32_t name_index = decode_integer(data, pos, 6);
                std::string name;
                if (name_index > 0 && name_index < 62) {
                    name = std::string(STATIC_TABLE[name_index].first);
                } else {
                    name = decode_string(data, pos);
                }
                std::string value = decode_string(data, pos);
                headers[std::move(name)] = std::move(value);
                // 동적 테이블 갱신은 무시 (최소 구현)
            } else if ((first & 0xF0) == 0x00) {
                // ── 리터럴 헤더 필드 — 인덱싱 없음 (RFC 7541 섹션 6.2.2) ──
                // 비트 패턴: 0000xxxx
                // 4비트 접두사로 이름 인덱스 디코딩
                uint32_t name_index = decode_integer(data, pos, 4);
                std::string name;
                if (name_index > 0 && name_index < 62) {
                    name = std::string(STATIC_TABLE[name_index].first);
                } else {
                    name = decode_string(data, pos);
                }
                std::string value = decode_string(data, pos);
                headers[std::move(name)] = std::move(value);
            } else if ((first & 0xF0) == 0x10) {
                // ── 리터럴 헤더 필드 — 절대 인덱싱 없음 (RFC 7541 섹션 6.2.3) ──
                // 비트 패턴: 0001xxxx
                // 4비트 접두사로 이름 인덱스 디코딩
                uint32_t name_index = decode_integer(data, pos, 4);
                std::string name;
                if (name_index > 0 && name_index < 62) {
                    name = std::string(STATIC_TABLE[name_index].first);
                } else {
                    name = decode_string(data, pos);
                }
                std::string value = decode_string(data, pos);
                headers[std::move(name)] = std::move(value);
            } else if ((first & 0xE0) == 0x20) {
                // ── 동적 테이블 크기 갱신 (RFC 7541 섹션 6.3) ──────────────
                // 비트 패턴: 001xxxxx — 무시
                decode_integer(data, pos, 5);
            } else {
                // 알 수 없는 표현 — 1바이트 건너뜀
                ++pos;
            }
        }

        return headers;
    }

private:
    /**
     * @brief HPACK 정수 인코딩 디코딩 (RFC 7541 섹션 5.1).
     *
     * @param data  원본 바이트 스팬.
     * @param pos   현재 읽기 위치 (in-out). 디코딩 후 다음 위치로 이동합니다.
     * @param prefix_bits 정수 접두사 비트 수 (1~8).
     * @returns 디코딩된 정수값.
     */
    static uint32_t decode_integer(std::span<const uint8_t> data,
                                   size_t& pos,
                                   uint8_t prefix_bits) {
        if (pos >= data.size()) return 0;

        const uint8_t mask = static_cast<uint8_t>((1u << prefix_bits) - 1u);
        uint32_t value = data[pos] & mask;
        ++pos;

        if (value < static_cast<uint32_t>(mask)) {
            // 접두사 내에 값이 완전히 들어옴
            return value;
        }

        // 멀티바이트 확장 (RFC 7541 섹션 5.1)
        uint32_t shift = 0;
        while (pos < data.size()) {
            uint8_t byte = data[pos++];
            value += static_cast<uint32_t>(byte & 0x7F) << shift;
            shift += 7;
            if ((byte & 0x80) == 0) break; // MSB == 0: 마지막 바이트
        }
        return value;
    }

    /**
     * @brief HPACK 문자열 리터럴 디코딩 (RFC 7541 섹션 5.2).
     *
     * Huffman 인코딩 미지원 — `H` 비트가 설정된 경우 원시 바이트로 반환합니다.
     *
     * @param data  원본 바이트 스팬.
     * @param pos   현재 읽기 위치 (in-out).
     * @returns 디코딩된 문자열.
     */
    static std::string decode_string(std::span<const uint8_t> data, size_t& pos) {
        if (pos >= data.size()) return {};

        // H 비트(최상위 비트): Huffman 인코딩 여부
        // bool huffman = (data[pos] & 0x80) != 0; // 현재 미지원
        uint32_t str_len = decode_integer(data, pos, 7);

        if (pos + str_len > data.size()) {
            // 버퍼 오버런 방지
            str_len = static_cast<uint32_t>(data.size() - pos);
        }

        std::string result(reinterpret_cast<const char*>(data.data() + pos), str_len);
        pos += str_len;
        return result;
    }

    /**
     * @brief HPACK 정적 테이블 (RFC 7541 부록 A).
     *
     * 인덱스 1~61에 해당하는 62개 항목 (0번은 미사용).
     * 처음 15개 항목을 포함하여 자주 사용되는 의사-헤더 및 일반 헤더를 정의합니다.
     */
    static const std::pair<std::string_view, std::string_view> STATIC_TABLE[62];
};

// HPACK 정적 테이블 정의 (RFC 7541 부록 A 전체 61개 항목)
inline const std::pair<std::string_view, std::string_view> HpackDecoder::STATIC_TABLE[62] = {
    // 인덱스 0: 미사용 (RFC 7541에서 인덱스는 1부터 시작)
    {"", ""},
    // 인덱스 1~15: 의사-헤더 및 자주 사용되는 헤더
    {":authority",                  ""},               //  1
    {":method",                     "GET"},             //  2
    {":method",                     "POST"},            //  3
    {":path",                       "/"},               //  4
    {":path",                       "/index.html"},     //  5
    {":scheme",                     "http"},            //  6
    {":scheme",                     "https"},           //  7
    {":status",                     "200"},             //  8
    {":status",                     "204"},             //  9
    {":status",                     "206"},             // 10
    {":status",                     "304"},             // 11
    {":status",                     "400"},             // 12
    {":status",                     "404"},             // 13
    {":status",                     "500"},             // 14
    {"accept-charset",              ""},               // 15
    // 인덱스 16~61: 일반 HTTP 헤더
    {"accept-encoding",             "gzip, deflate"},  // 16
    {"accept-language",             ""},               // 17
    {"accept-ranges",               ""},               // 18
    {"accept",                      ""},               // 19
    {"access-control-allow-origin", ""},               // 20
    {"age",                         ""},               // 21
    {"allow",                       ""},               // 22
    {"authorization",               ""},               // 23
    {"cache-control",               ""},               // 24
    {"content-disposition",         ""},               // 25
    {"content-encoding",            ""},               // 26
    {"content-language",            ""},               // 27
    {"content-length",              ""},               // 28
    {"content-location",            ""},               // 29
    {"content-range",               ""},               // 30
    {"content-type",                ""},               // 31
    {"cookie",                      ""},               // 32
    {"date",                        ""},               // 33
    {"etag",                        ""},               // 34
    {"expect",                      ""},               // 35
    {"expires",                     ""},               // 36
    {"from",                        ""},               // 37
    {"host",                        ""},               // 38
    {"if-match",                    ""},               // 39
    {"if-modified-since",           ""},               // 40
    {"if-none-match",               ""},               // 41
    {"if-range",                    ""},               // 42
    {"if-unmodified-since",         ""},               // 43
    {"last-modified",               ""},               // 44
    {"link",                        ""},               // 45
    {"location",                    ""},               // 46
    {"max-forwards",                ""},               // 47
    {"proxy-authenticate",          ""},               // 48
    {"proxy-authorization",         ""},               // 49
    {"range",                       ""},               // 50
    {"referer",                     ""},               // 51
    {"refresh",                     ""},               // 52
    {"retry-after",                 ""},               // 53
    {"server",                      ""},               // 54
    {"set-cookie",                  ""},               // 55
    {"strict-transport-security",   ""},               // 56
    {"transfer-encoding",           ""},               // 57
    {"user-agent",                  ""},               // 58
    {"vary",                        ""},               // 59
    {"via",                         ""},               // 60
    {"www-authenticate",            ""},               // 61
};

// ─── HpackEncoder ─────────────────────────────────────────────────────────────

/**
 * @brief HPACK 헤더 블록 인코더 (RFC 7541).
 *
 * 인덱스된 헤더 필드와 리터럴 헤더 필드(인덱싱 없음)를 혼합하여 인코딩합니다.
 * 동적 테이블은 사용하지 않습니다.
 *
 * ### 인코딩 전략
 * 1. 헤더 이름+값이 정적 테이블에 완전히 일치하면 → 인덱스 표현
 * 2. 헤더 이름만 정적 테이블에 일치하면 → 이름 인덱스 + 리터럴 값
 * 3. 그 외 → 리터럴 이름 + 리터럴 값 (인덱싱 없음)
 */
class HpackEncoder {
public:
    /**
     * @brief 헤더 맵을 HPACK 바이트 시퀀스로 인코딩합니다.
     *
     * @param headers 인코딩할 헤더 이름-값 쌍의 맵.
     * @returns HPACK으로 인코딩된 헤더 블록 바이트열.
     */
    std::vector<uint8_t> encode(const std::unordered_map<std::string, std::string>& headers) {
        std::vector<uint8_t> result;
        result.reserve(headers.size() * 32); // 평균 헤더 크기 추정

        for (const auto& [name, value] : headers) {
            // ── 1단계: 이름+값 완전 일치 검색 ──────────────────────────────
            uint32_t full_match_index = 0;
            uint32_t name_match_index = 0;

            for (uint32_t i = 1; i < 62; ++i) {
                const auto& [tname, tvalue] = HpackDecoder::STATIC_TABLE[i];
                if (tname == name) {
                    if (name_match_index == 0) name_match_index = i;
                    if (tvalue == value) {
                        full_match_index = i;
                        break;
                    }
                }
            }

            if (full_match_index > 0) {
                // ── 인덱스 헤더 필드 표현 (RFC 7541 섹션 6.1) ───────────────
                // 비트 패턴: 1xxxxxxx, 7비트 접두사
                encode_integer(result, full_match_index, 7, 0x80);
            } else if (name_match_index > 0) {
                // ── 리터럴 헤더 필드 — 인덱싱 없음, 이름 인덱스 참조 ────────
                // 비트 패턴: 0000xxxx, 4비트 접두사
                encode_integer(result, name_match_index, 4, 0x00);
                encode_string(result, value);
            } else {
                // ── 리터럴 헤더 필드 — 인덱싱 없음, 이름+값 모두 리터럴 ──────
                // 비트 패턴: 00000000 (이름 인덱스 = 0)
                result.push_back(0x00);
                encode_string(result, name);
                encode_string(result, value);
            }
        }

        return result;
    }

private:
    /**
     * @brief HPACK 정수 인코딩 (RFC 7541 섹션 5.1).
     *
     * @param out         출력 바이트 벡터.
     * @param value       인코딩할 정수값.
     * @param prefix_bits 접두사 비트 수 (1~8).
     * @param prefix_byte 접두사 바이트의 상위 비트 패턴 (접두사 비트 외 영역).
     */
    static void encode_integer(std::vector<uint8_t>& out,
                                uint32_t value,
                                uint8_t prefix_bits,
                                uint8_t prefix_byte) {
        const uint32_t max_prefix = (1u << prefix_bits) - 1u;

        if (value < max_prefix) {
            out.push_back(static_cast<uint8_t>(prefix_byte | value));
        } else {
            out.push_back(static_cast<uint8_t>(prefix_byte | max_prefix));
            value -= max_prefix;
            while (value >= 128) {
                out.push_back(static_cast<uint8_t>((value & 0x7F) | 0x80));
                value >>= 7;
            }
            out.push_back(static_cast<uint8_t>(value));
        }
    }

    /**
     * @brief HPACK 문자열 리터럴 인코딩 (RFC 7541 섹션 5.2, Huffman 미사용).
     *
     * @param out  출력 바이트 벡터.
     * @param str  인코딩할 문자열.
     */
    static void encode_string(std::vector<uint8_t>& out, std::string_view str) {
        // H 비트 = 0 (Huffman 미사용), 7비트 접두사로 길이 인코딩
        encode_integer(out, static_cast<uint32_t>(str.size()), 7, 0x00);
        out.insert(out.end(),
                   reinterpret_cast<const uint8_t*>(str.data()),
                   reinterpret_cast<const uint8_t*>(str.data()) + str.size());
    }
};

// ─── Http2Stream ──────────────────────────────────────────────────────────────

/**
 * @brief HTTP/2 단일 스트림의 상태와 데이터를 관리하는 구조체.
 *
 * HTTP/2 연결은 다수의 스트림을 다중화(multiplex)합니다.
 * 각 스트림은 RFC 7540 섹션 5.1에 정의된 상태 머신을 따릅니다.
 *
 * ### 스트림 상태 전이
 * ```
 *            idle
 *              |
 *         HEADERS 수신
 *              |
 *            open
 *           /    \
 *  END_STREAM   END_STREAM
 *  (원격에서)   (로컬에서)
 *      |              |
 * half_closed    half_closed
 *  (remote)       (local)
 *      \              /
 *       \            /
 *         closed
 * ```
 */
struct Http2Stream {
    /**
     * @brief 스트림 상태 열거형 (RFC 7540 섹션 5.1).
     */
    enum class State {
        IDLE,              ///< 초기 상태 — 아직 사용되지 않은 스트림
        OPEN,              ///< 양방향 통신 가능 상태
        HALF_CLOSED_LOCAL, ///< 로컬에서 END_STREAM 전송 완료 — 원격 수신만 가능
        HALF_CLOSED_REMOTE,///< 원격에서 END_STREAM 수신 완료 — 로컬 전송만 가능
        CLOSED,            ///< 스트림 완전 종료
    };

    uint32_t id{0};                ///< 스트림 식별자 (홀수: 클라이언트 개시, 짝수: 서버 개시)
    State    state{State::IDLE};   ///< 현재 스트림 상태

    /** @brief 수신된 요청 헤더 맵 (HPACK 디코딩 결과). */
    std::unordered_map<std::string, std::string> request_headers;

    /** @brief 수신된 요청 바디 바이트열. */
    std::vector<uint8_t> request_body;

    /** @brief 이 스트림에서 전송할 프레임 큐 (outgoing 채널). */
    std::shared_ptr<AsyncChannel<Http2Frame>> outgoing;
};

// ─── Http2Handler ─────────────────────────────────────────────────────────────

/**
 * @brief HTTP/2 연결 핸들러 (헤더-온리 구현).
 *
 * TLS-ALPN "h2" 협상 이후 HTTP/2 프레임의 수신/전송을 처리하는
 * 핵심 클래스입니다. C++20 코루틴(Task<>)을 사용하여 비동기로 동작합니다.
 *
 * ### 연결 처리 흐름
 * 1. `send_connection_preface()` — 서버 연결 프리페이스 + 초기 SETTINGS 전송
 * 2. `handle_frame()` — 네트워크에서 수신된 각 프레임을 처리
 *    - SETTINGS: ACK 응답 전송
 *    - PING: ACK 응답 전송
 *    - GOAWAY: 연결 종료 처리
 *    - HEADERS: HPACK 디코딩, 스트림 개시
 *    - DATA: 스트림 바디 수집
 *    - END_STREAM: 요청 핸들러 호출
 * 3. `send_headers()` / `send_data()` — 응답 전송
 * 4. `drain_pending_frames()` — 소켓에 쓸 대기 프레임 수거
 *
 * ### 사용 예시
 * @code
 * Http2Handler h2([](auto headers, auto body, auto stream) -> Task<void> {
 *     // 요청 처리 후 응답 전송
 *     co_await h2.send_headers(stream->id,
 *         {{":status", "200"}, {"content-type", "text/plain"}});
 *     co_await h2.send_data(stream->id,
 *         std::span<const uint8_t>(body), true);
 *     co_return;
 * });
 *
 * co_await h2.send_connection_preface();
 * // 네트워크 루프:
 * while (true) {
 *     Http2Frame frame = co_await read_frame(socket);
 *     co_await h2.handle_frame(std::move(frame));
 *     for (auto& f : h2.drain_pending_frames())
 *         co_await write_frame(socket, f);
 * }
 * @endcode
 */
class Http2Handler {
public:
    /**
     * @brief 완성된 HTTP/2 요청을 처리하는 애플리케이션 핸들러 타입.
     *
     * @param headers  HPACK 디코딩된 요청 헤더 맵.
     * @param body     요청 바디 바이트열.
     * @param stream   요청이 속한 스트림 (응답 전송에 사용).
     */
    using RequestHandler = std::function<Task<void>(
        std::unordered_map<std::string, std::string> headers,
        std::vector<uint8_t> body,
        std::shared_ptr<Http2Stream> stream)>;

    /**
     * @brief Http2Handler를 구성합니다.
     *
     * @param handler 완성된 HTTP/2 요청이 수신되면 호출될 애플리케이션 핸들러.
     */
    explicit Http2Handler(RequestHandler handler)
        : handler_(std::move(handler)) {}

    // ─── 프레임 수신 처리 ────────────────────────────────────────────────────

    /**
     * @brief 네트워크에서 수신된 원시 HTTP/2 프레임을 처리합니다.
     *
     * 프레임 타입에 따라 적절한 내부 처리 메서드로 디스패치합니다.
     * HEADERS+END_STREAM 또는 DATA+END_STREAM 수신 시 `handler_`를 호출합니다.
     *
     * @param frame 수신된 HTTP/2 프레임.
     * @returns 처리 결과. 프로토콜 에러 발생 시 에러 코드를 반환합니다.
     */
    Task<Result<void>> handle_frame(Http2Frame frame) {
        switch (frame.type) {
            case Http2FrameType::SETTINGS:
                co_return co_await handle_settings(frame);

            case Http2FrameType::PING:
                co_return co_await handle_ping(frame);

            case Http2FrameType::GOAWAY:
                co_return co_await handle_goaway(frame);

            case Http2FrameType::HEADERS:
                co_return co_await handle_headers(frame);

            case Http2FrameType::CONTINUATION:
                co_return co_await handle_continuation(frame);

            case Http2FrameType::DATA:
                co_return co_await handle_data(frame);

            case Http2FrameType::RST_STREAM:
                co_return co_await handle_rst_stream(frame);

            case Http2FrameType::WINDOW_UPDATE:
                // 흐름 제어 미구현 — 수신 무시
                co_return Result<void>::ok();

            case Http2FrameType::PRIORITY:
                // 우선순위 프레임 미구현 — 수신 무시
                co_return Result<void>::ok();

            case Http2FrameType::PUSH_PROMISE:
                // 클라이언트는 PUSH_PROMISE를 전송하지 않음 — 무시
                co_return Result<void>::ok();

            default:
                // 알 수 없는 프레임 타입 — RFC 7540 섹션 4.1: 무시
                co_return Result<void>::ok();
        }
    }

    // ─── 응답 전송 ───────────────────────────────────────────────────────────

    /**
     * @brief 지정된 스트림에 HEADERS 프레임을 전송합니다.
     *
     * 헤더 맵을 HPACK으로 인코딩하여 HEADERS 프레임을 `pending_frames_`에 추가합니다.
     *
     * @param stream_id  대상 스트림 식별자.
     * @param headers    전송할 헤더 이름-값 쌍 맵.
     * @param end_stream HEADERS 프레임에 END_STREAM 플래그 설정 여부.
     *                   true이면 이 프레임이 스트림의 마지막 프레임입니다.
     * @returns 처리 결과. 스트림이 존재하지 않으면 에러를 반환합니다.
     */
    Task<Result<void>> send_headers(
            uint32_t stream_id,
            const std::unordered_map<std::string, std::string>& headers,
            bool end_stream = false) {
        std::vector<uint8_t> encoded = hpack_encoder_.encode(headers);

        Http2Frame frame;
        frame.type      = Http2FrameType::HEADERS;
        frame.stream_id = stream_id;
        frame.flags     = HTTP2_FLAG_END_HEADERS;
        if (end_stream) frame.flags |= HTTP2_FLAG_END_STREAM;
        frame.payload   = std::move(encoded);
        frame.length    = static_cast<uint32_t>(frame.payload.size());

        pending_frames_.push_back(std::move(frame));

        // 스트림 상태 갱신
        if (auto it = streams_.find(stream_id); it != streams_.end()) {
            auto& stream = it->second;
            if (end_stream) {
                if (stream->state == Http2Stream::State::OPEN) {
                    stream->state = Http2Stream::State::HALF_CLOSED_LOCAL;
                } else if (stream->state == Http2Stream::State::HALF_CLOSED_REMOTE) {
                    stream->state = Http2Stream::State::CLOSED;
                }
            }
        }

        co_return Result<void>::ok();
    }

    /**
     * @brief 지정된 스트림에 DATA 프레임을 전송합니다.
     *
     * @param stream_id  대상 스트림 식별자.
     * @param data       전송할 바이트 데이터.
     * @param end_stream DATA 프레임에 END_STREAM 플래그 설정 여부.
     *                   true이면 이 프레임이 스트림의 마지막 데이터입니다.
     * @returns 처리 결과.
     */
    Task<Result<void>> send_data(
            uint32_t stream_id,
            std::span<const uint8_t> data,
            bool end_stream = true) {
        Http2Frame frame;
        frame.type      = Http2FrameType::DATA;
        frame.stream_id = stream_id;
        frame.flags     = end_stream ? HTTP2_FLAG_END_STREAM : 0;
        frame.payload.assign(data.begin(), data.end());
        frame.length    = static_cast<uint32_t>(frame.payload.size());

        pending_frames_.push_back(std::move(frame));

        // 스트림 상태 갱신
        if (end_stream) {
            if (auto it = streams_.find(stream_id); it != streams_.end()) {
                auto& stream = it->second;
                if (stream->state == Http2Stream::State::OPEN) {
                    stream->state = Http2Stream::State::HALF_CLOSED_LOCAL;
                } else if (stream->state == Http2Stream::State::HALF_CLOSED_REMOTE) {
                    stream->state = Http2Stream::State::CLOSED;
                }
            }
        }

        co_return Result<void>::ok();
    }

    // ─── 대기 프레임 수거 ────────────────────────────────────────────────────

    /**
     * @brief 소켓에 쓸 대기 프레임을 모두 수거하여 반환합니다.
     *
     * 반환 후 내부 `pending_frames_` 버퍼는 비워집니다.
     * 호출자는 반환된 프레임들을 직렬화하여 소켓에 써야 합니다.
     *
     * @returns 전송 대기 중인 프레임 벡터. 비어 있을 수 있습니다.
     */
    std::vector<Http2Frame> drain_pending_frames() {
        std::vector<Http2Frame> out;
        out.swap(pending_frames_);
        return out;
    }

    // ─── 연결 수준 프레임 전송 ───────────────────────────────────────────────

    /**
     * @brief SETTINGS 프레임을 `pending_frames_`에 추가합니다.
     *
     * @param ack true이면 ACK SETTINGS 프레임을 전송합니다 (페이로드 없음).
     *            false이면 서버 기본 설정 파라미터를 포함한 SETTINGS를 전송합니다.
     */
    Task<void> send_settings(bool ack = false) {
        Http2Frame frame;
        frame.type      = Http2FrameType::SETTINGS;
        frame.stream_id = 0; // 연결 수준 프레임은 항상 stream_id == 0

        if (ack) {
            // ACK SETTINGS: 플래그만 설정, 페이로드 없음
            frame.flags   = HTTP2_FLAG_ACK;
            frame.length  = 0;
        } else {
            // 서버 초기 SETTINGS 파라미터 전송
            // 각 파라미터: 2바이트 식별자 + 4바이트 값 = 6바이트
            frame.flags = 0;
            auto& p = frame.payload;

            // HEADER_TABLE_SIZE (0x1) = 4096
            append_uint16(p, 0x1);
            append_uint32(p, DEFAULT_HEADER_TABLE_SIZE);

            // MAX_FRAME_SIZE (0x5) = 16384
            append_uint16(p, 0x5);
            append_uint32(p, DEFAULT_MAX_FRAME_SIZE);

            // INITIAL_WINDOW_SIZE (0x4) = 65535
            append_uint16(p, 0x4);
            append_uint32(p, DEFAULT_INITIAL_WINDOW_SIZE);

            frame.length = static_cast<uint32_t>(p.size());
        }

        pending_frames_.push_back(std::move(frame));
        co_return;
    }

    /**
     * @brief PING 프레임을 `pending_frames_`에 추가합니다.
     *
     * @param ack          true이면 수신된 PING에 대한 ACK 응답을 전송합니다.
     * @param opaque_data  PING 페이로드 8바이트 불투명 데이터.
     *                     ACK 시 수신된 값을 그대로 반환해야 합니다.
     */
    Task<void> send_ping(bool ack, uint64_t opaque_data = 0) {
        Http2Frame frame;
        frame.type      = Http2FrameType::PING;
        frame.stream_id = 0;
        frame.flags     = ack ? HTTP2_FLAG_ACK : 0;
        frame.length    = 8; // PING 페이로드는 항상 8바이트

        // 8바이트 opaque data (빅 엔디안)
        frame.payload.resize(8);
        for (int i = 7; i >= 0; --i) {
            frame.payload[static_cast<size_t>(i)] =
                static_cast<uint8_t>(opaque_data & 0xFF);
            opaque_data >>= 8;
        }

        pending_frames_.push_back(std::move(frame));
        co_return;
    }

    /**
     * @brief GOAWAY 프레임을 `pending_frames_`에 추가하고 연결 종료를 알립니다.
     *
     * @param last_stream_id 서버가 성공적으로 처리한 마지막 스트림 ID.
     * @param error_code     종료 원인 에러 코드 (RFC 7540 섹션 7).
     * @param message        추가 디버그 메시지 (선택적).
     */
    Task<void> send_goaway(uint32_t last_stream_id,
                            uint32_t error_code,
                            std::string_view message = "") {
        Http2Frame frame;
        frame.type      = Http2FrameType::GOAWAY;
        frame.stream_id = 0;
        frame.flags     = 0;

        // 페이로드: Last-Stream-ID(4바이트) + Error Code(4바이트) + 추가 데이터
        append_uint32(frame.payload, last_stream_id & 0x7FFFFFFF); // MSB 예약
        append_uint32(frame.payload, error_code);
        if (!message.empty()) {
            frame.payload.insert(frame.payload.end(),
                reinterpret_cast<const uint8_t*>(message.data()),
                reinterpret_cast<const uint8_t*>(message.data()) + message.size());
        }

        frame.length = static_cast<uint32_t>(frame.payload.size());
        pending_frames_.push_back(std::move(frame));
        co_return;
    }

    // ─── 연결 프리페이스 ─────────────────────────────────────────────────────

    /**
     * @brief HTTP/2 서버 연결 프리페이스를 전송합니다.
     *
     * TLS 핸드셰이크 및 ALPN "h2" 협상 완료 후 즉시 호출해야 합니다.
     * 클라이언트는 "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" 프리페이스를 전송하고,
     * 서버는 초기 SETTINGS 프레임으로 응답합니다 (RFC 7540 섹션 3.5).
     *
     * @returns 처리 결과. 항상 성공을 반환합니다.
     */
    Task<Result<void>> send_connection_preface() {
        // 서버 연결 프리페이스: 초기 SETTINGS 프레임 전송
        co_await send_settings(false);
        co_return Result<void>::ok();
    }

    // ─── 직렬화 유틸리티 (정적) ──────────────────────────────────────────────

    /**
     * @brief HTTP/2 프레임을 9바이트 헤더 + 페이로드 바이트열로 직렬화합니다.
     *
     * RFC 7540 섹션 4.1의 프레임 포맷을 따릅니다.
     *
     * @param frame 직렬화할 프레임.
     * @returns 직렬화된 바이트열 (9바이트 헤더 + 페이로드).
     */
    static std::vector<uint8_t> serialize_frame(const Http2Frame& frame) {
        std::vector<uint8_t> out;
        out.reserve(9 + frame.payload.size());

        // Length (24비트, 빅 엔디안)
        out.push_back(static_cast<uint8_t>((frame.length >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((frame.length >>  8) & 0xFF));
        out.push_back(static_cast<uint8_t>( frame.length        & 0xFF));

        // Type (8비트)
        out.push_back(static_cast<uint8_t>(frame.type));

        // Flags (8비트)
        out.push_back(frame.flags);

        // Stream Identifier (31비트, MSB 예약 = 0, 빅 엔디안)
        const uint32_t sid = frame.stream_id & 0x7FFFFFFF;
        out.push_back(static_cast<uint8_t>((sid >> 24) & 0xFF));
        out.push_back(static_cast<uint8_t>((sid >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((sid >>  8) & 0xFF));
        out.push_back(static_cast<uint8_t>( sid        & 0xFF));

        // Payload
        out.insert(out.end(), frame.payload.begin(), frame.payload.end());

        return out;
    }

private:
    // ─── 내부 프레임 처리 메서드 ─────────────────────────────────────────────

    /**
     * @brief SETTINGS 프레임을 처리합니다 (RFC 7540 섹션 6.5).
     *
     * ACK 플래그가 없는 SETTINGS를 수신하면 ACK로 응답합니다.
     * ACK SETTINGS는 무시합니다.
     *
     * @param frame 수신된 SETTINGS 프레임.
     * @returns 처리 결과.
     */
    Task<Result<void>> handle_settings(const Http2Frame& frame) {
        if (frame.stream_id != 0) {
            // SETTINGS는 반드시 stream_id == 0이어야 함 (PROTOCOL_ERROR)
            co_await send_goaway(last_stream_id_, 0x1 /* PROTOCOL_ERROR */,
                                 "SETTINGS with non-zero stream ID");
            co_return Result<void>::err(
                std::make_error_code(std::errc::protocol_error));
        }

        if (frame.flags & HTTP2_FLAG_ACK) {
            // ACK SETTINGS 수신 — 무시
            co_return Result<void>::ok();
        }

        // 피어 SETTINGS 파라미터 처리 (현재 무시, 최소 구현)
        // ACK 응답 전송
        co_await send_settings(true);
        co_return Result<void>::ok();
    }

    /**
     * @brief PING 프레임을 처리합니다 (RFC 7540 섹션 6.7).
     *
     * ACK 플래그가 없는 PING을 수신하면 동일한 opaque_data로 ACK 응답합니다.
     *
     * @param frame 수신된 PING 프레임.
     * @returns 처리 결과.
     */
    Task<Result<void>> handle_ping(const Http2Frame& frame) {
        if (frame.flags & HTTP2_FLAG_ACK) {
            // ACK PING 수신 — 무시 (RTT 측정 등은 미구현)
            co_return Result<void>::ok();
        }

        // 수신된 opaque_data 그대로 반환
        uint64_t opaque = 0;
        if (frame.payload.size() >= 8) {
            for (int i = 0; i < 8; ++i) {
                opaque = (opaque << 8) | frame.payload[static_cast<size_t>(i)];
            }
        }
        co_await send_ping(true, opaque);
        co_return Result<void>::ok();
    }

    /**
     * @brief GOAWAY 프레임을 처리합니다 (RFC 7540 섹션 6.8).
     *
     * 피어가 연결 종료를 요청한 경우로, 연결을 우아하게 종료합니다.
     *
     * @param frame 수신된 GOAWAY 프레임.
     * @returns 처리 결과.
     */
    Task<Result<void>> handle_goaway(const Http2Frame& /*frame*/) {
        // 피어 GOAWAY 수신: 진행 중인 스트림 정리 후 연결 종료
        // 최소 구현: 에러 코드 반환으로 상위 레이어에 알림
        co_return Result<void>::err(
            std::make_error_code(std::errc::connection_aborted));
    }

    /**
     * @brief HEADERS 프레임을 처리합니다 (RFC 7540 섹션 6.2).
     *
     * 새 스트림을 개시하거나 기존 스트림의 트레일러 헤더를 처리합니다.
     * END_HEADERS 플래그가 설정된 경우 HPACK 디코딩을 완료합니다.
     * END_STREAM 플래그가 설정된 경우 요청 핸들러를 호출합니다.
     *
     * @param frame 수신된 HEADERS 프레임.
     * @returns 처리 결과.
     */
    Task<Result<void>> handle_headers(const Http2Frame& frame) {
        const uint32_t sid = frame.stream_id;
        if (sid == 0) {
            // HEADERS는 반드시 stream_id != 0이어야 함
            co_await send_goaway(last_stream_id_, 0x1 /* PROTOCOL_ERROR */,
                                 "HEADERS with stream_id == 0");
            co_return Result<void>::err(
                std::make_error_code(std::errc::protocol_error));
        }

        // 스트림 생성 또는 조회
        auto& stream = get_or_create_stream(sid);

        // PRIORITY 플래그: 페이로드 앞 5바이트 건너뜀
        size_t header_block_offset = 0;
        if (frame.flags & HTTP2_FLAG_PRIORITY) {
            header_block_offset = 5;
        }
        // PADDED 플래그: 첫 바이트가 패딩 길이
        uint8_t pad_length = 0;
        if (frame.flags & HTTP2_FLAG_PADDED) {
            if (frame.payload.empty()) {
                co_return Result<void>::err(
                    std::make_error_code(std::errc::protocol_error));
            }
            pad_length = frame.payload[0];
            header_block_offset += 1;
        }

        // 헤더 블록 프래그먼트 추출
        if (header_block_offset > frame.payload.size()) {
            co_return Result<void>::err(
                std::make_error_code(std::errc::protocol_error));
        }
        size_t payload_end = frame.payload.size() - pad_length;
        if (payload_end < header_block_offset) {
            co_return Result<void>::err(
                std::make_error_code(std::errc::protocol_error));
        }

        std::span<const uint8_t> header_block{
            frame.payload.data() + header_block_offset,
            payload_end - header_block_offset
        };

        if (frame.flags & HTTP2_FLAG_END_HEADERS) {
            // 헤더 블록 완료 — HPACK 디코딩
            auto decoded = hpack_decoder_.decode(header_block);
            for (auto& [k, v] : decoded) {
                stream->request_headers[k] = std::move(v);
            }
            stream->state = Http2Stream::State::OPEN;
        } else {
            // CONTINUATION 프레임 대기 — 헤더 블록 버퍼에 저장
            // (최소 구현: CONTINUATION 프레임에서 계속 처리)
            continuation_buffer_[sid].insert(
                continuation_buffer_[sid].end(),
                header_block.begin(),
                header_block.end());
        }

        if (frame.flags & HTTP2_FLAG_END_STREAM) {
            stream->state = Http2Stream::State::HALF_CLOSED_REMOTE;
            // 핸들러 호출
            if (handler_) {
                auto t = handler_(stream->request_headers,
                                  stream->request_body,
                                  stream);
                t.detach();
            }
        }

        last_stream_id_ = sid;
        co_return Result<void>::ok();
    }

    /**
     * @brief CONTINUATION 프레임을 처리합니다 (RFC 7540 섹션 6.10).
     *
     * 이전 HEADERS 또는 PUSH_PROMISE 프레임에서 시작된 헤더 블록의
     * 연속 데이터를 수신합니다.
     *
     * @param frame 수신된 CONTINUATION 프레임.
     * @returns 처리 결과.
     */
    Task<Result<void>> handle_continuation(const Http2Frame& frame) {
        const uint32_t sid = frame.stream_id;
        auto it = continuation_buffer_.find(sid);
        if (it == continuation_buffer_.end()) {
            // 예상치 못한 CONTINUATION — 프로토콜 에러
            co_await send_goaway(last_stream_id_, 0x1 /* PROTOCOL_ERROR */,
                                 "Unexpected CONTINUATION frame");
            co_return Result<void>::err(
                std::make_error_code(std::errc::protocol_error));
        }

        // 헤더 블록 버퍼에 추가
        auto& buf = it->second;
        buf.insert(buf.end(), frame.payload.begin(), frame.payload.end());

        if (frame.flags & HTTP2_FLAG_END_HEADERS) {
            // 헤더 블록 완료 — HPACK 디코딩
            auto stream_it = streams_.find(sid);
            if (stream_it != streams_.end()) {
                auto decoded = hpack_decoder_.decode(
                    std::span<const uint8_t>(buf.data(), buf.size()));
                for (auto& [k, v] : decoded) {
                    stream_it->second->request_headers[k] = std::move(v);
                }
                stream_it->second->state = Http2Stream::State::OPEN;
            }
            continuation_buffer_.erase(it);
        }

        co_return Result<void>::ok();
    }

    /**
     * @brief DATA 프레임을 처리합니다 (RFC 7540 섹션 6.1).
     *
     * 스트림 바디를 수집합니다.
     * END_STREAM 플래그가 설정된 경우 요청 핸들러를 호출합니다.
     *
     * @param frame 수신된 DATA 프레임.
     * @returns 처리 결과.
     */
    Task<Result<void>> handle_data(const Http2Frame& frame) {
        const uint32_t sid = frame.stream_id;
        if (sid == 0) {
            // DATA는 반드시 stream_id != 0이어야 함
            co_await send_goaway(last_stream_id_, 0x1 /* PROTOCOL_ERROR */,
                                 "DATA with stream_id == 0");
            co_return Result<void>::err(
                std::make_error_code(std::errc::protocol_error));
        }

        auto it = streams_.find(sid);
        if (it == streams_.end()) {
            // 알 수 없는 스트림 — RST_STREAM으로 응답
            co_await send_rst_stream(sid, 0x1 /* PROTOCOL_ERROR */);
            co_return Result<void>::ok();
        }

        auto& stream = it->second;

        // PADDED 플래그 처리
        size_t data_offset = 0;
        size_t data_end    = frame.payload.size();
        if (frame.flags & HTTP2_FLAG_PADDED) {
            if (frame.payload.empty()) {
                co_return Result<void>::err(
                    std::make_error_code(std::errc::protocol_error));
            }
            uint8_t pad = frame.payload[0];
            data_offset = 1;
            if (data_end < static_cast<size_t>(pad) + 1) {
                co_return Result<void>::err(
                    std::make_error_code(std::errc::protocol_error));
            }
            data_end -= pad;
        }

        // 바디 데이터 수집
        stream->request_body.insert(
            stream->request_body.end(),
            frame.payload.begin() + static_cast<ptrdiff_t>(data_offset),
            frame.payload.begin() + static_cast<ptrdiff_t>(data_end));

        if (frame.flags & HTTP2_FLAG_END_STREAM) {
            stream->state = Http2Stream::State::HALF_CLOSED_REMOTE;
            // 핸들러 호출
            if (handler_) {
                auto t = handler_(stream->request_headers,
                                  stream->request_body,
                                  stream);
                t.detach();
            }
        }

        co_return Result<void>::ok();
    }

    /**
     * @brief RST_STREAM 프레임을 처리합니다 (RFC 7540 섹션 6.4).
     *
     * 피어가 스트림을 강제 종료한 경우 스트림 상태를 CLOSED로 변경합니다.
     *
     * @param frame 수신된 RST_STREAM 프레임.
     * @returns 처리 결과.
     */
    Task<Result<void>> handle_rst_stream(const Http2Frame& frame) {
        const uint32_t sid = frame.stream_id;
        if (auto it = streams_.find(sid); it != streams_.end()) {
            it->second->state = Http2Stream::State::CLOSED;
        }
        co_return Result<void>::ok();
    }

    /**
     * @brief RST_STREAM 프레임을 전송합니다.
     *
     * @param stream_id  종료할 스트림 식별자.
     * @param error_code 종료 원인 에러 코드 (RFC 7540 섹션 7).
     */
    Task<void> send_rst_stream(uint32_t stream_id, uint32_t error_code) {
        Http2Frame frame;
        frame.type      = Http2FrameType::RST_STREAM;
        frame.stream_id = stream_id;
        frame.flags     = 0;
        frame.length    = 4;
        append_uint32(frame.payload, error_code);
        pending_frames_.push_back(std::move(frame));
        co_return;
    }

    // ─── 스트림 관리 유틸리티 ────────────────────────────────────────────────

    /**
     * @brief 스트림 ID로 스트림을 조회하거나 새로 생성합니다.
     *
     * @param sid 스트림 식별자.
     * @returns 기존 또는 새로 생성된 스트림의 shared_ptr 참조.
     */
    std::shared_ptr<Http2Stream>& get_or_create_stream(uint32_t sid) {
        auto it = streams_.find(sid);
        if (it == streams_.end()) {
            auto stream = std::make_shared<Http2Stream>();
            stream->id       = sid;
            stream->state    = Http2Stream::State::IDLE;
            stream->outgoing = std::make_shared<AsyncChannel<Http2Frame>>(64);
            streams_[sid]    = stream;
            return streams_[sid];
        }
        return it->second;
    }

    // ─── 직렬화 헬퍼 (정적) ──────────────────────────────────────────────────

    /**
     * @brief uint16_t를 빅 엔디안으로 벡터에 추가합니다.
     * @param v   대상 벡터.
     * @param val 추가할 16비트 정수값.
     */
    static void append_uint16(std::vector<uint8_t>& v, uint16_t val) {
        v.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        v.push_back(static_cast<uint8_t>( val       & 0xFF));
    }

    /**
     * @brief uint32_t를 빅 엔디안으로 벡터에 추가합니다.
     * @param v   대상 벡터.
     * @param val 추가할 32비트 정수값.
     */
    static void append_uint32(std::vector<uint8_t>& v, uint32_t val) {
        v.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
        v.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
        v.push_back(static_cast<uint8_t>((val >>  8) & 0xFF));
        v.push_back(static_cast<uint8_t>( val        & 0xFF));
    }

    // ─── 멤버 변수 ───────────────────────────────────────────────────────────

    /** @brief 완성된 HTTP/2 요청을 처리하는 애플리케이션 핸들러. */
    RequestHandler handler_;

    /** @brief 스트림 ID → 스트림 객체 맵. */
    std::unordered_map<uint32_t, std::shared_ptr<Http2Stream>> streams_;

    /** @brief HPACK 헤더 블록 디코더 인스턴스. */
    HpackDecoder hpack_decoder_;

    /** @brief HPACK 헤더 블록 인코더 인스턴스. */
    HpackEncoder hpack_encoder_;

    /** @brief 소켓에 쓸 대기 프레임 큐. `drain_pending_frames()`로 수거합니다. */
    std::vector<Http2Frame> pending_frames_;

    /** @brief 서버가 처리한 마지막 클라이언트 스트림 ID (GOAWAY에 사용). */
    uint32_t last_stream_id_{0};

    /**
     * @brief CONTINUATION 프레임 수신 중인 스트림의 헤더 블록 버퍼.
     *
     * 스트림 ID → 아직 완성되지 않은 헤더 블록 바이트열.
     * END_HEADERS 플래그 수신 시 HPACK 디코딩 후 삭제됩니다.
     */
    std::unordered_map<uint32_t, std::vector<uint8_t>> continuation_buffer_;

    // ─── SETTINGS 기본값 상수 ────────────────────────────────────────────────

    /** @brief 기본 HPACK 헤더 테이블 크기 (RFC 7541 섹션 6.5.2). */
    static constexpr uint32_t DEFAULT_HEADER_TABLE_SIZE  = 4096;

    /** @brief 기본 최대 프레임 크기 (RFC 7540 섹션 6.5.2). */
    static constexpr uint32_t DEFAULT_MAX_FRAME_SIZE     = 16384;

    /** @brief 기본 초기 흐름 제어 윈도우 크기 (RFC 7540 섹션 6.5.2). */
    static constexpr uint32_t DEFAULT_INITIAL_WINDOW_SIZE = 65535;
};

} // namespace qbuem

/** @} */ // end of qbuem_http2_handler
