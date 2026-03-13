#pragma once

/**
 * @file draco/middleware/jwt.hpp
 * @brief Compatibility shim → use <draco/middleware/token_auth.hpp>.
 *
 * qbuem-stack is a zero-dependency framework.
 * The concrete OpenSSL/HMAC-SHA256 implementation has been removed.
 * Inject your own verifier via ITokenVerifier.
 *
 * See <draco/middleware/token_auth.hpp>.
 */

#include <draco/middleware/token_auth.hpp>
