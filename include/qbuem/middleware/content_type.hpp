#pragma once

/**
 * @file qbuem/middleware/content_type.hpp
 * @brief Content-Type enforcement middleware.
 *
 * Provides middleware that rejects requests whose Content-Type header does not
 * match the required value, returning 415 Unsupported Media Type.
 *
 * ## Usage
 * @code
 * // Reject POST/PUT/PATCH requests without application/json Content-Type
 * app.use(qbuem::middleware::require_json());
 *
 * // Generic version — require multipart/form-data for POST only
 * app.use(qbuem::middleware::require_content_type(
 *     "multipart/form-data",
 *     {qbuem::Method::Post}
 * ));
 * @endcode
 */

#include <qbuem/http/request.hpp>
#include <qbuem/http/response.hpp>
#include <qbuem/http/router.hpp>

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace qbuem::middleware {

/**
 * @brief Rejects requests whose Content-Type does not contain the required value.
 *
 * Only checks requests whose method is in `methods`. Other methods are passed
 * through unchanged.  On rejection, sends 415 Unsupported Media Type and halts
 * the middleware chain (returns false).
 *
 * @param content_type  Expected Content-Type substring (e.g. "application/json").
 * @param methods       HTTP methods to enforce the check on.
 *                      Defaults to POST, PUT, PATCH.
 */
inline Middleware require_content_type(
    std::string_view content_type,
    std::initializer_list<Method> methods = {Method::Post, Method::Put, Method::Patch})
{
    return [ct = std::string(content_type),
            ms = std::vector<Method>(methods)](const Request &req,
                                               Response &res) -> bool {
        bool applies = false;
        for (Method m : ms) {
            if (req.method() == m) { applies = true; break; }
        }
        if (!applies) return true;

        std::string_view ct_hdr = req.header("Content-Type");
        if (ct_hdr.find(ct) != std::string_view::npos) return true;

        res.status(415).body("Unsupported Media Type: expected " + ct);
        return false;
    };
}

/**
 * @brief Rejects POST/PUT/PATCH requests that lack an application/json Content-Type.
 *
 * Returns 415 Unsupported Media Type and halts the middleware chain.
 *
 * @code
 * app.use(qbuem::middleware::require_json());
 * @endcode
 */
inline Middleware require_json() {
    return require_content_type("application/json");
}

} // namespace qbuem::middleware
