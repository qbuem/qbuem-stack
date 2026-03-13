#pragma once

/**
 * @file qbuem/middleware/jwt.hpp
 * @brief Compatibility shim → use <qbuem/middleware/token_auth.hpp>.
 *
 * qbuem-stack is a zero-dependency framework.
 * The concrete OpenSSL/HMAC-SHA256 implementation has been removed.
 * Inject your own verifier via ITokenVerifier.
 *
 * See <qbuem/middleware/token_auth.hpp>.
 */

#include <qbuem/middleware/token_auth.hpp>
