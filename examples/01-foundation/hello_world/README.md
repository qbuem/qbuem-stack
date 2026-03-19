# hello_world

**Category:** Foundation
**File:** `hello_world.cpp`
**Complexity:** Beginner

## Overview

The simplest possible qbuem-stack HTTP application. Demonstrates how to create an `App`, register middleware and route handlers, and bind to a TCP port — all in under 35 lines of C++23.

## Scenario

A developer evaluating qbuem-stack for the first time wants to verify that the framework compiles, links, and serves HTTP responses correctly. This example covers the minimal path from `main()` to a running server.

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────────┐
│  HTTP Client                                                │
│  (curl / browser)                                           │
└──────────────────────────┬──────────────────────────────────┘
                           │  TCP :8080
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  qbuem::App (io_uring / kqueue)                             │
│                                                             │
│  ┌─────────────────────────────────────────────────────┐   │
│  │  Middleware Chain                                   │   │
│  │  └─ [LOG]  logs method + path to stdout            │   │
│  └─────────────────────────────────────────────────────┘   │
│                           │                                 │
│  ┌─────────────────────────────────────────────────────┐   │
│  │  Router (RadixTree)                                 │   │
│  │  ├─ GET /          → "Hello from qbuem-stack!"     │   │
│  │  └─ GET /user/:id  → "User profile requested"      │   │
│  └─────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `qbuem::App` | Top-level server builder and event loop |
| `app.use(middleware)` | Register a request middleware |
| `app.get(path, handler)` | Register a GET route handler |
| `app.listen(port)` | Bind and start accepting connections |
| `Request::method()` | Retrieve HTTP method |
| `Request::path()` | Retrieve request URI path |
| `Response::status(code).body(str)` | Build and send an HTTP response |

## Input

| Source | Detail |
|--------|--------|
| `GET /` | Returns a plain-text greeting |
| `GET /user/:id` | Path parameter capture (`:id` is ignored in this demo) |

## Output

```
# Server startup
# (Listening on 0.0.0.0:8080)

# Per request (to stdout)
[LOG] 1 /                  ← method enum value (1 = GET)
[LOG] 1 /user/42

# HTTP responses
HTTP/1.1 200 OK
Content-Length: 23
Hello from qbuem-stack!

HTTP/1.1 200 OK
Content-Length: 21
User profile requested
```

## How to Run

```bash
# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target hello_world

# Run
./build/examples/01-foundation/hello_world/hello_world

# Test (in another terminal)
curl http://localhost:8080/
curl http://localhost:8080/user/42
```

## Expected Behavior

- Server listens indefinitely until killed with `Ctrl+C`.
- Every request is logged to stdout before the handler runs.
- Unknown routes receive a 404 response automatically.

## Notes

- No coroutines are used; both handlers are synchronous lambdas.
- The logging middleware returns `true`, which allows the request to continue down the chain.
- See `async_timer` for the coroutine-based handler variant.
