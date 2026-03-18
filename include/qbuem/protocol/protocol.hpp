#pragma once

/**
 * @file qbuem/protocol/protocol.hpp
 * @brief qbuem-stack protocol handler umbrella header.
 * @defgroup qbuem_protocol Protocol Handlers
 * @ingroup qbuem
 *
 * Including this single header provides access to all protocol handlers.
 *
 * ```cpp
 * #include <qbuem/protocol/protocol.hpp>
 * // Http1Handler, Http2Handler, WebSocketHandler, GrpcHandler all available
 * ```
 *
 * ### Included handlers
 * - `Http1Handler`        — HTTP/1.1 keep-alive, chunked, Upgrade
 * - `Http2Handler`        — HTTP/2 HPACK + stream multiplexing
 * - `WebSocketHandler`    — RFC 6455 WebSocket (HTTP Upgrade)
 * - `GrpcHandler<Req,Res>` — gRPC Unary/Server/Client/Bidi streaming
 */

#include <qbuem/protocol/http1_handler.hpp>
#include <qbuem/protocol/http2_handler.hpp>
#include <qbuem/protocol/websocket_handler.hpp>
#include <qbuem/protocol/grpc_handler.hpp>
