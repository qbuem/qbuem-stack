# tcp_echo_server

**Category:** Network
**File:** `tcp_echo_server.cpp`
**Complexity:** Intermediate

## Overview

A non-blocking TCP echo server built with `TcpListener`, `TcpStream`, and a `Dispatcher`. Incoming connections are accepted in an async coroutine loop; each connection runs its own echo coroutine that reads data and writes it back verbatim.

## Scenario

Demonstrate the raw TCP programming model in qbuem-stack: binding a listener, accepting multiple concurrent connections, performing non-blocking read/write in coroutines, and tracking per-server statistics (total connections, echo bytes).

## Architecture Diagram

```
  ┌──────────┐     TCP :18080    ┌──────────────────────────────┐
  │ Client 1 │ ───────────────►  │  TcpListener (127.0.0.1:     │
  └──────────┘                   │              18080)           │
  ┌──────────┐                   │                              │
  │ Client 2 │ ───────────────►  │  accept_loop() coroutine     │
  └──────────┘                   │     │                        │
  ┌──────────┐                   │     ├─► dispatcher.spawn()   │
  │ Client N │ ───────────────►  │     │       │                │
  └──────────┘                   │     │       ▼                │
                                 │     │  handle_connection()   │
                                 │     │    coroutine           │
                                 │     │    ┌──────────────┐   │
                                 │     │    │ co_await read │   │
                                 │     │    │ co_await write│   │
                                 │     │    │  (echo back)  │   │
                                 │     │    └──────────────┘   │
                                 │     │                        │
                                 │  Dispatcher (1 thread)       │
                                 │  g_connections / g_echo_bytes│
                                 └──────────────────────────────┘

 Auto-shutdown after 5 seconds
 Final stats printed to stdout
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `SocketAddr::from_ipv4(ip, port)` | Create a typed socket address |
| `TcpListener::bind(addr)` | Bind and start listening |
| `co_await listener.accept()` | Async accept — yields until a client connects |
| `co_await stream.read(buf)` | Async read into a byte buffer |
| `co_await stream.write(span)` | Async write from a byte span |
| `dispatcher.spawn(coro)` | Schedule a coroutine on the dispatcher |
| `Dispatcher::run()` | Start the event loop (blocks the calling thread) |
| `Dispatcher::stop()` | Signal the event loop to drain and exit |

## Input

| Source | Detail |
|--------|--------|
| Any TCP client on `127.0.0.1:18080` | Sends arbitrary bytes |
| `nc 127.0.0.1 18080` | Suggested test tool |

## Output

```
=== TCP Echo Server (port 18080) ===
[tcp] Listening on 127.0.0.1:18080
[tcp] (Connect with: nc 127.0.0.1 18080)
[tcp] Running for 5 seconds then shutting down...
[tcp] New connection accepted       ← per connection
[tcp] Stats: connections=2 echo_bytes=27
```

Data received from each client is echoed back byte-for-byte.

## How to Run

```bash
# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target tcp_echo_server

# Run server
./build/examples/02-network/tcp_echo_server/tcp_echo_server

# Test (in another terminal)
echo "hello echo" | nc 127.0.0.1 18080
```

## Expected Behavior

1. Server binds `127.0.0.1:18080` and enters the accept loop.
2. Each accepted connection spawns a new `handle_connection` coroutine.
3. That coroutine reads up to 1 024 bytes, writes them back, and loops.
4. After 5 seconds, `g_stop` is set; the accept loop exits and the dispatcher stops.
5. Final connection and byte counts are printed.

## Notes

- The read buffer is stack-allocated (`std::array<std::byte, 1024>`).
- `g_stop` is a `std::atomic<bool>` checked at each loop iteration; no additional cancellation token is needed for this simple demo.
- For production usage, bind `0.0.0.0` and increase the buffer size.
- See `udp_unix_socket` for UDP and Unix-domain socket variants.
