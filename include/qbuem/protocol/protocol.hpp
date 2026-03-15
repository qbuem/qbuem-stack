#pragma once

/**
 * @file qbuem/protocol/protocol.hpp
 * @brief qbuem-stack 프로토콜 핸들러 umbrella 헤더.
 * @defgroup qbuem_protocol Protocol Handlers
 * @ingroup qbuem
 *
 * 이 헤더를 하나만 포함하면 모든 프로토콜 핸들러에 접근할 수 있습니다.
 *
 * ```cpp
 * #include <qbuem/protocol/protocol.hpp>
 * // Http1Handler, Http2Handler, WebSocketHandler, GrpcHandler 모두 사용 가능
 * ```
 *
 * ### 포함되는 핸들러
 * - `Http1Handler`        — HTTP/1.1 keep-alive, chunked, Upgrade
 * - `Http2Handler`        — HTTP/2 HPACK + 스트림 멀티플렉싱
 * - `WebSocketHandler`    — RFC 6455 WebSocket (HTTP Upgrade)
 * - `GrpcHandler<Req,Res>` — gRPC Unary/Server/Client/Bidi streaming
 */

#include <qbuem/protocol/http1_handler.hpp>
#include <qbuem/protocol/http2_handler.hpp>
#include <qbuem/protocol/websocket_handler.hpp>
#include <qbuem/protocol/grpc_handler.hpp>
