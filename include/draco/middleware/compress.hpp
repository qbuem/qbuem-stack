#pragma once

/**
 * @file draco/middleware/compress.hpp
 * @brief Compatibility shim → use <draco/middleware/body_encoder.hpp>.
 *
 * qbuem-stack is a zero-dependency framework.
 * The concrete zlib implementation has been removed.
 * Inject your own encoder via IBodyEncoder.
 *
 * See <draco/middleware/body_encoder.hpp>.
 */

#include <draco/middleware/body_encoder.hpp>
