#pragma once

/**
 * @file qbuem/buf/io_slice.hpp
 * @brief [Compatibility] I/O buffer primitive types — split into individual headers.
 * @ingroup qbuem_buf
 *
 * @deprecated This header is retained for backward compatibility.
 * New code should include the individual headers directly:
 *
 * ```cpp
 * // Recommended (v1.0+)
 * #include <qbuem/io/io_slice.hpp>   // IOSlice, MutableIOSlice
 * #include <qbuem/io/iovec.hpp>      // IOVec<N>
 * #include <qbuem/io/read_buf.hpp>   // ReadBuf<N>
 * #include <qbuem/io/write_buf.hpp>  // WriteBuf
 * ```
 *
 * This file includes all four headers above.
 */

#include <qbuem/io/io_slice.hpp>
#include <qbuem/io/iovec.hpp>
#include <qbuem/io/read_buf.hpp>
#include <qbuem/io/write_buf.hpp>
