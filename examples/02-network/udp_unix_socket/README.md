# udp_unix_socket

**Category:** Network
**File:** `udp_unix_socket_example.cpp`
**Complexity:** Intermediate

## Overview

Demonstrates `UdpSocket` (connectionless UDP datagram I/O) and `UnixSocket` (Unix-domain socket IPC) together with `SocketAddr`. Shows send/receive flows for both transport types in a single binary.

## Scenario

A system component needs to emit lightweight telemetry datagrams over UDP while also communicating with a local sidecar process via a Unix-domain socket. This example covers both patterns.

## Architecture Diagram

```
  UDP Demo
  ─────────────────────────────────────────────────────────
  ┌─────────────┐   sendto()   ┌─────────────────────────┐
  │ UdpSocket   │ ──────────►  │  UdpSocket (recv)       │
  │ (sender)    │   127.0.0.1  │  :19991                 │
  │ :19990      │   :19991     └─────────────────────────┘
  └─────────────┘
                 ◄──────────   recvfrom() → reply sent back

  Unix-Domain Socket Demo
  ─────────────────────────────────────────────────────────
  ┌─────────────┐  sendto()    ┌─────────────────────────┐
  │ UnixSocket  │ ──────────►  │  UnixSocket (server)    │
  │ (client)    │  /tmp/qbuem  │  /tmp/qbuem_test.sock   │
  └─────────────┘  _test.sock  └─────────────────────────┘
                                reply written back
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `SocketAddr::from_ipv4(ip, port)` | IPv4 address + port |
| `SocketAddr::from_unix(path)` | Unix-domain socket path |
| `UdpSocket::bind(addr)` | Create and bind a UDP socket |
| `UdpSocket::sendto(data, addr)` | Send datagram to peer |
| `UdpSocket::recvfrom(buf)` | Receive datagram and source address |
| `UnixSocket::bind(path)` | Bind a UNIX datagram socket |
| `UnixSocket::sendto(data, path)` | Send to a UNIX socket path |
| `UnixSocket::recvfrom(buf)` | Receive from UNIX socket |

## Input / Output

### UDP Flow

| Step | Direction | Data |
|------|-----------|------|
| `send` | sender → receiver | `"Hello UDP!"` |
| `recv` | receiver receives | `"Hello UDP!"` + sender addr |
| `reply` | receiver → sender | `"UDP ACK"` |

### Unix Domain Socket Flow

| Step | Direction | Data |
|------|-----------|------|
| `send` | client → server | `"Hello Unix!"` |
| `recv` | server receives | `"Hello Unix!"` |
| `reply` | server → client | `"Unix ACK"` |

## How to Run

```bash
# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target udp_unix_socket_example

# Run (self-contained, no external peer needed)
./build/examples/02-network/udp_unix_socket/udp_unix_socket_example
```

## Expected Output

```
=== UDP Socket Demo ===
[udp] Sent: Hello UDP!
[udp] Received: Hello UDP! from 127.0.0.1:19990
[udp] Reply received: UDP ACK

=== Unix Domain Socket Demo ===
[unix] Sent: Hello Unix!
[unix] Received: Hello Unix!
[unix] Reply received: Unix ACK
```

## Notes

- Both sender and receiver run in the same process for simplicity; in production they would be in separate processes or threads.
- The Unix socket file is cleaned up (`unlink`) at the end of the example.
- Prefer `UdpSocket` for network telemetry and `UnixSocket` for same-host IPC where latency matters.
