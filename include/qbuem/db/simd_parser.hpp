#pragma once

/**
 * @file qbuem/db/simd_parser.hpp
 * @brief SIMD-accelerated DB protocol parser (PostgreSQL / MySQL wire format).
 * @defgroup qbuem_db_simd SIMDParser
 * @ingroup qbuem_db
 *
 * ## Overview
 * Accelerates result set parsing of DB wire protocols using SIMD (AVX-512 / NEON) instructions.
 *
 * ## Supported Backends
 * - **PostgreSQL**: Frontend/Backend Protocol v3 (DataRow messages)
 * - **MySQL**: Protocol 41 (Text / Binary ResultSet)
 *
 * ## Optimization Principles
 * 1. **Stateless Parsing**: Parser operates only on the input buffer, with no external state.
 * 2. **Zero-copy Output**: String/blob fields reference the input buffer directly as views.
 * 3. **SIMD Scan**: Column boundaries (`\x00`, `\xff`) are scanned in bulk via SIMD,
 *    yielding 4-16x throughput improvement over scalar loops.
 * 4. **Compile-time Dispatch**: `if constexpr` + compiler intrinsics minimize runtime branching.
 *
 * ## Per-Platform SIMD Paths
 * | Platform         | SIMD Level | Register Width |
 * |------------------|------------|----------------|
 * | x86-64 (AVX-512) | 512-bit    | 64B            |
 * | x86-64 (AVX2)    | 256-bit    | 32B            |
 * | x86-64 (SSE4.2)  | 128-bit    | 16B            |
 * | AArch64 (NEON)   | 128-bit    | 16B            |
 * | Fallback         | Scalar     | 1B             |
 *
 * @note Actual AVX-512/NEON calls require compile flags (`-mavx512f`, `-march=armv8-a`)
 *       and `#include <immintrin.h>` / `<arm_neon.h>` at link time.
 *       This header provides ABI-stable wrappers; the implementation resides in `db/simd_parser.cpp`.
 * @{
 */

#include <qbuem/common.hpp>
#include <qbuem/db/value.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace qbuem::db {

// ─── CPU feature detection ────────────────────────────────────────────────────

/** @brief Enumeration indicating the available SIMD level at runtime. */
enum class SimdLevel : uint8_t {
    Scalar  = 0,   ///< No SIMD (fallback)
    SSE42   = 1,   ///< x86 SSE 4.2 (16B)
    AVX2    = 2,   ///< x86 AVX2 (32B)
    AVX512  = 3,   ///< x86 AVX-512 (64B)
    Neon    = 4,   ///< AArch64 NEON (16B)
};

/**
 * @brief Detects the available SIMD level on the current CPU at runtime.
 * @returns Detected SimdLevel.
 */
[[nodiscard]] SimdLevel detect_simd_level() noexcept;

// ─── Parsing context ──────────────────────────────────────────────────────────

/**
 * @brief Parsing session context (stateless).
 *
 * All fields are managed solely as offsets/views into the input buffer (`src`).
 * No heap allocation occurs; the parser creates this struct on the stack.
 */
struct ParseCtx {
    const uint8_t* src;    ///< Input buffer pointer (non-owning)
    size_t         len;    ///< Input buffer length
    size_t         pos;    ///< Current parsing position
    uint16_t       ncols;  ///< Number of columns (read from DataRow header)

    /** @brief Returns the number of remaining bytes. */
    [[nodiscard]] size_t remaining() const noexcept {
        return pos < len ? len - pos : 0;
    }

    /** @brief Returns a view from the current position. */
    [[nodiscard]] BufferView rest() const noexcept {
        return {src + pos, remaining()};
    }
};

// ─── Parsing results ──────────────────────────────────────────────────────────

/**
 * @brief Single field parsing result (zero-copy).
 *
 * `data` directly references the input buffer — no copy.
 */
struct ParsedField {
    BufferView  data;     ///< Field byte view (empty span if NULL)
    bool        is_null;  ///< Whether the value is SQL NULL
};

/**
 * @brief Single row parsing result.
 *
 * Stores up to `kMaxCols` columns on the stack (no heap allocation).
 */
struct ParsedRow {
    static constexpr uint16_t kMaxCols = 256;

    ParsedField fields[kMaxCols];
    uint16_t    count{0};
};

// ─── PostgreSQL parser ────────────────────────────────────────────────────────

/**
 * @brief PostgreSQL Frontend/Backend Protocol v3 DataRow parser.
 *
 * ## Message Format
 * ```
 * DataRow ('D') {
 *     int8  type    = 'D'
 *     int32 length          // message length (self-inclusive)
 *     int16 field_count
 *     for each field:
 *         int32 col_len     // -1 = NULL
 *         byte[col_len] data
 * }
 * ```
 *
 * ## SIMD Optimization
 * - AVX-512: Bulk-loads `int32` field lengths in 64-byte chunks.
 * - AVX2/SSE: Scans in 16-32 byte units.
 * - NEON: Processes 16 bytes at a time using `vld1q_u8`.
 */
class PgDataRowParser {
public:
    /** @brief Constructor: detects SIMD level and selects the optimal parser. */
    PgDataRowParser() noexcept;

    /**
     * @brief Parses a DataRow message.
     *
     * @param ctx  Parsing context. `pos` is the start position of the DataRow message.
     * @param row  Output row struct (stack-allocated).
     * @returns true on success; false if the message is incomplete (more data needed).
     *
     * @note `ctx.pos` advances by the number of bytes consumed.
     */
    [[nodiscard]] bool parse_row(ParseCtx& ctx, ParsedRow& row) const noexcept;

    /** @brief Returns the SIMD level currently in use. */
    [[nodiscard]] SimdLevel simd_level() const noexcept { return level_; }

private:
    SimdLevel level_;

    // Scalar fallback
    static bool parse_row_scalar(ParseCtx& ctx, ParsedRow& row) noexcept;

    // SSE4.2 path
    static bool parse_row_sse42(ParseCtx& ctx, ParsedRow& row) noexcept;

    // AVX2 path
    static bool parse_row_avx2(ParseCtx& ctx, ParsedRow& row) noexcept;

    // AVX-512 path (x86 only)
    static bool parse_row_avx512(ParseCtx& ctx, ParsedRow& row) noexcept;

    // NEON path (AArch64 only)
    static bool parse_row_neon(ParseCtx& ctx, ParsedRow& row) noexcept;
};

// ─── MySQL parser ─────────────────────────────────────────────────────────────

/**
 * @brief MySQL Protocol 41 Binary ResultSet parser.
 *
 * ## Message Format (Binary Protocol Row)
 * ```
 * BinaryRow {
 *     int8  packet_header = 0x00
 *     byte[ceil(ncols/8)] null_bitmap
 *     for each non-null column:
 *         <type-specific encoding>
 * }
 * ```
 *
 * ## SIMD Optimization
 * - NULL bitmap is processed in bulk using AVX2/NEON to vectorize column NULL checks.
 * - Variable-length encoding (`length-encoded integer`) is decoded in parallel.
 */
class MySQLBinaryRowParser {
public:
    /** @brief Constructor: detects SIMD level and selects the optimal parser. */
    MySQLBinaryRowParser() noexcept;

    /**
     * @brief Parses a Binary ResultSet row.
     * @param ctx   Parsing context.
     * @param ncols Number of columns.
     * @param row   Output row struct.
     * @returns true on success, false on failure.
     */
    [[nodiscard]] bool parse_row(ParseCtx& ctx, uint16_t ncols,
                                  ParsedRow& row) const noexcept;

    /** @brief Returns the SIMD level currently in use. */
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

// ─── Helper: ParsedField → db::Value conversion ───────────────────────────────

/**
 * @brief Converts a `ParsedField` to a `db::Value` (zero-copy).
 *
 * By default, returns the text representation as `db::Value::Text`.
 * If a type hint (`pg_oid`, `mysql_type`) is provided, converts accordingly.
 *
 * @param field  Parsed field.
 * @param pg_oid PostgreSQL type OID (0 if unknown).
 * @returns `db::Value`.
 */
[[nodiscard]] Value field_to_value(const ParsedField& field,
                                    uint32_t pg_oid = 0) noexcept;

} // namespace qbuem::db

/** @} */
