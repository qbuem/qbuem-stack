#pragma once

/**
 * @file qbuem/db/simd_parser.hpp
 * @brief SIMD 가속 DB 프로토콜 파서 (PostgreSQL / MySQL 와이어 포맷).
 * @defgroup qbuem_db_simd SIMDParser
 * @ingroup qbuem_db
 *
 * ## 개요
 * DB 와이어 프로토콜의 결과셋 파싱을 SIMD(AVX-512 / NEON) 명령어로 가속합니다.
 *
 * ## 지원 백엔드
 * - **PostgreSQL**: Frontend/Backend Protocol v3 (DataRow 메시지)
 * - **MySQL**: Protocol 41 (Text / Binary ResultSet)
 *
 * ## 최적화 원칙
 * 1. **Stateless Parsing**: 파서는 외부 상태 없이 입력 버퍼만으로 처리.
 * 2. **Zero-copy Output**: 문자열/블롭 필드는 입력 버퍼를 직접 뷰로 참조.
 * 3. **SIMD Scan**: 컬럼 경계(`\x00`, `\xff`)를 SIMD로 일괄 탐색하여
 *    스칼라 루프 대비 4-16배 처리량 향상.
 * 4. **Compile-time Dispatch**: `if constexpr` + 컴파일러 내장 함수(intrinsics)로
 *    실행 시간 분기 최소화.
 *
 * ## 플랫폼별 SIMD 경로
 * | 플랫폼 | SIMD 레벨 | 레지스터 폭 |
 * |--------|-----------|------------|
 * | x86-64 (AVX-512) | 512-bit | 64B |
 * | x86-64 (AVX2)    | 256-bit | 32B |
 * | x86-64 (SSE4.2)  | 128-bit | 16B |
 * | AArch64 (NEON)   | 128-bit | 16B |
 * | Fallback         | 스칼라  | 1B  |
 *
 * @note 실제 AVX-512/NEON 호출을 위해서는 컴파일 플래그(`-mavx512f`, `-march=armv8-a`)
 *       및 링크 시 `#include <immintrin.h>` / `<arm_neon.h>`가 필요합니다.
 *       이 헤더는 ABI-안정 래퍼를 제공하며, 구현은 `db/simd_parser.cpp`에 위치합니다.
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/db/value.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace qbuem::db {

// ─── CPU 기능 감지 ────────────────────────────────────────────────────────────

/** @brief 런타임에 SIMD 가용 수준을 나타내는 열거형. */
enum class SimdLevel : uint8_t {
    Scalar  = 0,   ///< SIMD 없음 (폴백)
    SSE42   = 1,   ///< x86 SSE 4.2 (16B)
    AVX2    = 2,   ///< x86 AVX2 (32B)
    AVX512  = 3,   ///< x86 AVX-512 (64B)
    Neon    = 4,   ///< AArch64 NEON (16B)
};

/**
 * @brief 현재 CPU에서 가용한 SIMD 수준을 런타임에 감지합니다.
 * @returns 감지된 SimdLevel.
 */
[[nodiscard]] SimdLevel detect_simd_level() noexcept;

// ─── 파싱 컨텍스트 ────────────────────────────────────────────────────────────

/**
 * @brief 파싱 세션 컨텍스트 (stateless).
 *
 * 모든 필드는 입력 버퍼(`src`)의 오프셋/뷰로만 관리됩니다.
 * 힙 할당이 없으며, 파서는 이 구조체를 스택에 생성합니다.
 */
struct ParseCtx {
    const uint8_t* src;    ///< 입력 버퍼 포인터 (소유하지 않음)
    size_t         len;    ///< 입력 버퍼 길이
    size_t         pos;    ///< 현재 파싱 위치
    uint16_t       ncols;  ///< 컬럼 수 (DataRow 헤더에서 읽음)

    /** @brief 남은 바이트 수를 반환합니다. */
    [[nodiscard]] size_t remaining() const noexcept {
        return pos < len ? len - pos : 0;
    }

    /** @brief 현재 위치부터의 뷰를 반환합니다. */
    [[nodiscard]] BufferView rest() const noexcept {
        return {src + pos, remaining()};
    }
};

// ─── 파싱 결과 ────────────────────────────────────────────────────────────────

/**
 * @brief 단일 필드 파싱 결과 (zero-copy).
 *
 * `data`는 입력 버퍼를 직접 참조합니다 — 복사 없음.
 */
struct ParsedField {
    BufferView  data;     ///< 필드 바이트 뷰 (NULL이면 empty span)
    bool        is_null;  ///< SQL NULL 여부
};

/**
 * @brief 단일 행(row) 파싱 결과.
 *
 * 최대 `kMaxCols` 컬럼을 스택에 저장 (힙 미사용).
 */
struct ParsedRow {
    static constexpr uint16_t kMaxCols = 256;

    ParsedField fields[kMaxCols];
    uint16_t    count{0};
};

// ─── PostgreSQL 파서 ─────────────────────────────────────────────────────────

/**
 * @brief PostgreSQL Frontend/Backend Protocol v3 DataRow 파서.
 *
 * ## 메시지 포맷
 * ```
 * DataRow ('D') {
 *     int8  type    = 'D'
 *     int32 length          // 메시지 길이 (자기 포함)
 *     int16 field_count
 *     for each field:
 *         int32 col_len     // -1 = NULL
 *         byte[col_len] data
 * }
 * ```
 *
 * ## SIMD 최적화
 * - AVX-512: 64바이트 단위로 `int32` 필드 길이를 일괄 로드.
 * - AVX2/SSE: 16-32바이트 단위 스캔.
 * - NEON: `vld1q_u8`으로 16바이트 단위 처리.
 */
class PgDataRowParser {
public:
    /** @brief 생성자: SIMD 수준을 감지하고 최적 파서를 선택합니다. */
    PgDataRowParser() noexcept;

    /**
     * @brief DataRow 메시지를 파싱합니다.
     *
     * @param ctx  파싱 컨텍스트. `pos`는 DataRow 메시지 시작 위치.
     * @param row  출력 행 구조체 (스택 할당).
     * @returns 파싱 성공 시 true; 불완전한 메시지면 false (더 많은 데이터 필요).
     *
     * @note `ctx.pos`는 소비된 바이트만큼 전진합니다.
     */
    [[nodiscard]] bool parse_row(ParseCtx& ctx, ParsedRow& row) const noexcept;

    /** @brief 사용 중인 SIMD 수준을 반환합니다. */
    [[nodiscard]] SimdLevel simd_level() const noexcept { return level_; }

private:
    SimdLevel level_;

    // 스칼라 폴백
    static bool parse_row_scalar(ParseCtx& ctx, ParsedRow& row) noexcept;

    // SSE4.2 경로
    static bool parse_row_sse42(ParseCtx& ctx, ParsedRow& row) noexcept;

    // AVX2 경로
    static bool parse_row_avx2(ParseCtx& ctx, ParsedRow& row) noexcept;

    // AVX-512 경로 (x86 전용)
    static bool parse_row_avx512(ParseCtx& ctx, ParsedRow& row) noexcept;

    // NEON 경로 (AArch64 전용)
    static bool parse_row_neon(ParseCtx& ctx, ParsedRow& row) noexcept;
};

// ─── MySQL 파서 ──────────────────────────────────────────────────────────────

/**
 * @brief MySQL Protocol 41 Binary ResultSet 파서.
 *
 * ## 메시지 포맷 (Binary Protocol Row)
 * ```
 * BinaryRow {
 *     int8  packet_header = 0x00
 *     byte[ceil(ncols/8)] null_bitmap
 *     for each non-null column:
 *         <type-specific encoding>
 * }
 * ```
 *
 * ## SIMD 최적화
 * - NULL 비트맵을 AVX2/NEON으로 일괄 처리하여 컬럼 NULL 여부를 벡터화.
 * - 가변 길이 인코딩 (`length-encoded integer`)을 병렬 디코딩.
 */
class MySQLBinaryRowParser {
public:
    /** @brief 생성자: SIMD 수준을 감지하고 최적 파서를 선택합니다. */
    MySQLBinaryRowParser() noexcept;

    /**
     * @brief Binary ResultSet 행을 파싱합니다.
     * @param ctx   파싱 컨텍스트.
     * @param ncols 컬럼 수.
     * @param row   출력 행 구조체.
     * @returns 파싱 성공 여부.
     */
    [[nodiscard]] bool parse_row(ParseCtx& ctx, uint16_t ncols,
                                  ParsedRow& row) const noexcept;

    /** @brief 사용 중인 SIMD 수준을 반환합니다. */
    [[nodiscard]] SimdLevel simd_level() const noexcept { return level_; }

private:
    SimdLevel level_;

    static bool parse_row_scalar(ParseCtx& ctx, uint16_t ncols,
                                  ParsedRow& row) noexcept;
    static bool parse_row_sse42(ParseCtx& ctx, uint16_t ncols,
                                 ParsedRow& row) noexcept;
    static bool parse_row_avx2(ParseCtx& ctx, uint16_t ncols,
                                ParsedRow& row) noexcept;
    static bool parse_row_neon(ParseCtx& ctx, uint16_t ncols,
                                ParsedRow& row) noexcept;
};

// ─── 헬퍼: ParsedField → db::Value 변환 ─────────────────────────────────────

/**
 * @brief `ParsedField`를 `db::Value`로 변환합니다 (zero-copy).
 *
 * 기본적으로 텍스트 표현을 `db::Value::Text`로 반환합니다.
 * 타입 힌트(`pg_oid`, `mysql_type`)가 있으면 적절히 변환합니다.
 *
 * @param field  파싱된 필드.
 * @param pg_oid PostgreSQL 타입 OID (알 수 없으면 0).
 * @returns `db::Value`.
 */
[[nodiscard]] Value field_to_value(const ParsedField& field,
                                    uint32_t pg_oid = 0) noexcept;

} // namespace qbuem::db

/** @} */
