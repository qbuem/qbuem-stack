#pragma once

/**
 * @file qbuem/middleware/compress.hpp
 * @brief Compatibility shim → use <qbuem/middleware/body_encoder.hpp>.
 *
 * qbuem-stack is a zero-dependency framework.
 * The concrete zlib implementation has been removed.
 * Inject your own encoder via IBodyEncoder.
 *
 * See <qbuem/middleware/body_encoder.hpp>.
 */

#include <qbuem/middleware/body_encoder.hpp>
