# websocket

**Category:** Network
**File:** `websocket_example.cpp`
**Complexity:** Intermediate

## Overview

Demonstrates the WebSocket protocol layer in qbuem-stack: RFC 6455 handshake key computation, frame encoding/decoding for Text, Binary, Ping, and Close frame types, and client-to-server masking.

## Scenario

A backend service needs to implement a WebSocket echo endpoint. This example shows the frame-level mechanics that underpin the higher-level WebSocket handler, including the accept-key derivation used during the HTTP Upgrade handshake and the masked binary frame round-trip.

## Architecture Diagram

```
  HTTP Upgrade Handshake
  ──────────────────────────────────────────────────────────────
  Client                          Server
  ──────────────────────────────────────────────────────────────
  GET /ws HTTP/1.1
  Upgrade: websocket
  Sec-WebSocket-Key: <base64>  ──►  compute_accept_key(key)
                                      SHA-1(key + GUID) → base64
                               ◄──  101 Switching Protocols
                                    Sec-WebSocket-Accept: <base64>

  Frame Encode / Decode (post-handshake)
  ──────────────────────────────────────────────────────────────
  encode_frame(WsFrame)          →  wire bytes
  decode_frame(wire bytes)       →  WsFrame

  Frame types covered:
  ┌──────────┬──────────┬─────────────────────────────────────┐
  │ Type     │ Masked   │ Description                         │
  ├──────────┼──────────┼─────────────────────────────────────┤
  │ Text     │ No       │ Server → client text payload        │
  │ Binary   │ Yes      │ Client → server binary (unmasked)   │
  │ Ping     │ No       │ Server keep-alive probe             │
  │ Close    │ No       │ Normal closure (code 1000)          │
  └──────────┴──────────┴─────────────────────────────────────┘
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `WebSocketHandler::compute_accept_key(key)` | Derive `Sec-WebSocket-Accept` header value |
| `WebSocketHandler::encode_frame(frame)` | Serialize a `WsFrame` to wire bytes |
| `WebSocketHandler::decode_frame(span, consumed)` | Deserialize bytes to `WsFrame` |
| `WsFrame::opcode` | Frame type: `Text`, `Binary`, `Ping`, `Pong`, `Close` |
| `WsFrame::fin` | Final fragment flag |
| `WsFrame::masked` | Client-to-server masking flag |
| `WsFrame::payload` | Frame payload bytes |

## Input

| Function | Input |
|----------|-------|
| `websocket_echo_example()` | Hard-coded RFC 6455 sample key + "Hello, WebSocket!" text |
| `masked_frame_example()` | Binary payload `{0xDE, 0xAD, 0xBE, 0xEF}` with masking enabled |
| `upgrade_validation_example()` | Simulated Upgrade header flags |

## Output

```
[ws] Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
[ws] Encoded frame bytes: 19
[ws] Decoded: opcode=1 fin=1 payload='Hello, WebSocket!'
[ws] Ping frame bytes: 2
[ws] Close frame bytes: 4
[ws] Masked binary frame: 8 bytes
[ws] Unmasked payload: de ad be ef
[ws] WebSocket handshake validated
```

## How to Run

```bash
# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target websocket_example

# Run
./build/examples/02-network/websocket/websocket_example
```

## Frame Wire Format (RFC 6455)

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
├─┼─┼─┼─┼─────┼─┼───────────────┼───────────────────────────────┤
│F│R│R│R│opco │M│  Payload len  │    Extended payload length     │
│I│S│S│S│de   │A│  (7 bits)     │    (16 or 64 bits, optional)  │
│N│V│V│V│     │S│               │                                │
│ │1│2│3│     │K│               │                                │
├─┴─┴─┴─┴─────┴─┴───────────────┴────────────────────────────────┤
│             Masking-key (4 bytes, if MASK bit set)              │
├─────────────────────────────────────────────────────────────────┤
│                        Payload Data                             │
└─────────────────────────────────────────────────────────────────┘
```

## Notes

- The accepted key must match `s3pPLMBiTxaQ9kYGzzhZRbK+xOo=` for the RFC 6455 test vector.
- Client frames **must** be masked (RFC 6455 §5.3); server frames must **not** be masked.
- `decode_frame` returns a `Result<WsFrame>`; check for errors before reading the payload.
