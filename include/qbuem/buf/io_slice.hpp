#pragma once

/**
 * @file qbuem/buf/io_slice.hpp
 * @brief [Compatibility] I/O 버퍼 기본 타입 집합 — 개별 헤더로 분리됨.
 * @ingroup qbuem_buf
 *
 * @deprecated 이 헤더는 하위 호환성을 위해 유지됩니다.
 * 새 코드에서는 개별 헤더를 직접 포함하는 것을 권장합니다:
 *
 * ```cpp
 * // 권장 방식 (v1.0+)
 * #include <qbuem/io/io_slice.hpp>   // IOSlice, MutableIOSlice
 * #include <qbuem/io/iovec.hpp>      // IOVec<N>
 * #include <qbuem/io/read_buf.hpp>   // ReadBuf<N>
 * #include <qbuem/io/write_buf.hpp>  // WriteBuf
 * ```
 *
 * 이 파일은 위 4개 헤더를 모두 포함합니다.
 */

#include <qbuem/io/io_slice.hpp>
#include <qbuem/io/iovec.hpp>
#include <qbuem/io/read_buf.hpp>
#include <qbuem/io/write_buf.hpp>
