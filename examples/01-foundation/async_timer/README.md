# async_timer

**Category:** Foundation
**File:** `async_timer.cpp`
**Complexity:** Beginner

## Overview

Demonstrates both synchronous and asynchronous (coroutine-based) HTTP handlers side-by-side. The `/sleep` endpoint uses `co_await sleep(ms)` to pause for 1 second without blocking the event loop thread.

## Scenario

A developer needs to understand how qbuem-stack handles non-blocking I/O delays inside HTTP handlers. This example shows that a coroutine-based handler can yield the thread back to the reactor during a sleep, allowing other requests to be served concurrently.

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────────┐
│  HTTP Client A  ─── GET /hello ───►  sync handler          │
│                                      └─ returns immediately │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│  HTTP Client B  ─── GET /sleep ───►  async coroutine        │
│                                      ├─ co_await sleep(1s)  │
│                                      │   (thread NOT blocked│
│                                      │    other reqs served)│
│                                      └─ resumes after 1s    │
└─────────────────────────────────────────────────────────────┘

         ┌──────────────────────┐
         │  qbuem::App          │
         │  Reactor event loop  │
         │  (single thread)     │
         │                      │
         │  ┌────────────────┐  │
         │  │ /hello handler │  │
         │  │  (sync)        │  │
         │  └────────────────┘  │
         │  ┌────────────────┐  │
         │  │ /sleep handler │  │
         │  │  (coroutine)   │  │
         │  │  co_await sleep│  │
         │  └────────────────┘  │
         └──────────────────────┘
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `Handler(fn)` | Wrap a synchronous callback as a route handler |
| `AsyncHandler(coro)` | Wrap a coroutine lambda as an async route handler |
| `co_await sleep(ms)` | Non-blocking timer await inside a coroutine handler |
| `Task<void>` | Return type for async handlers |
| `co_return` | Exit a coroutine |

## Input

| Endpoint | Handler Type | Description |
|----------|-------------|-------------|
| `GET /hello` | Synchronous | Returns immediately |
| `GET /sleep` | Async coroutine | Waits 1 second, then responds |

## Output

```
Draco WAS Async Timer Example running on http://0.0.0.0:8080
Try: curl http://localhost:8080/sleep

# When /hello is requested:
HTTP/1.1 200 OK
Hello from sync handler!

# When /sleep is requested:
[Server] Handling /sleep request...
# ... 1 second pause ...
[Server] Resuming after 1s sleep
HTTP/1.1 200 OK
Hello after 1s sleep!
```

## How to Run

```bash
# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target async_timer

# Run
./build/examples/01-foundation/async_timer/async_timer

# Test concurrency: send /sleep and /hello at the same time
curl http://localhost:8080/sleep &
curl http://localhost:8080/hello
```

## Expected Behavior

- `/hello` responds instantly regardless of concurrent `/sleep` requests.
- `/sleep` suspends the coroutine for exactly 1 second via the reactor's timer, then resumes and writes the response.
- The event loop thread is never blocked — both requests are handled from the same thread.

## Notes

- `co_await sleep(1000)` translates to a timer registration in the reactor; the coroutine is re-scheduled when the timer fires.
- This is the recommended pattern for I/O-bound waits inside route handlers.
