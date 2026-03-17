# middleware

**Category:** Advanced Applications
**File:** `middleware_example.cpp`
**Complexity:** Intermediate
**Dependencies:** `qbuem-json`

## Overview

Demonstrates the full HTTP middleware stack: CORS, RateLimit, RequestID injection, HSTS (HTTP Strict Transport Security), Bearer auth, and SSE (Server-Sent Events) middleware — all in a single integrated example.

## Scenario

A production REST API needs:
- **CORS** for browser-based frontends.
- **Rate limiting** to protect against abuse.
- **Request ID** for distributed tracing correlation.
- **HSTS** to enforce HTTPS.
- **Bearer auth** to protect privileged endpoints.
- **SSE** to push real-time updates to connected clients.

## Architecture Diagram

```
  HTTP Request
       │
       ▼
  ┌──────────────────────────────────────────────────────────┐
  │  Middleware Chain (evaluated in order)                   │
  │                                                          │
  │  1. RequestIDMiddleware                                  │
  │     ├─ reads X-Request-ID header                        │
  │     └─ injects new UUID if absent                       │
  │                                                          │
  │  2. HSTSMiddleware                                       │
  │     └─ adds Strict-Transport-Security header            │
  │                                                          │
  │  3. CORSMiddleware                                       │
  │     ├─ handles OPTIONS preflight                        │
  │     └─ adds Access-Control-Allow-* headers              │
  │                                                          │
  │  4. RateLimitMiddleware                                  │
  │     ├─ token bucket per IP                              │
  │     └─ returns 429 Too Many Requests if exceeded        │
  │                                                          │
  │  5. BearerAuthMiddleware (protected routes only)        │
  │     ├─ extracts Authorization: Bearer <token>           │
  │     └─ returns 401 if invalid                           │
  │                                                          │
  │  6. Route Handler                                        │
  └──────────────────────────────────────────────────────────┘

  SSE Route: GET /events
  ──────────────────────────────────────────────────────────
  Client connects → SSE upgrade
  Server pushes: "data: {\"type\":\"update\", ...}\n\n"
  MessageBus → SSEMiddleware → all connected SSE clients
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `CORSMiddleware(config)` | Cross-Origin Resource Sharing |
| `RateLimitMiddleware(config)` | Token bucket per IP |
| `RequestIDMiddleware` | Inject/propagate X-Request-ID |
| `HSTSMiddleware(max_age)` | Strict-Transport-Security header |
| `BearerAuthMiddleware(token)` | Bearer token validation |
| `SSEMiddleware` | Server-Sent Events stream |
| `app.use(mw)` | Register middleware |

## HTTP Headers Added

| Middleware | Header | Example Value |
|------------|--------|--------------|
| RequestID | `X-Request-ID` | `"req-8f3a..."` |
| HSTS | `Strict-Transport-Security` | `max-age=31536000` |
| CORS | `Access-Control-Allow-Origin` | `"https://app.example.com"` |
| CORS | `Access-Control-Allow-Methods` | `"GET, POST, OPTIONS"` |

## How to Run

```bash
# Requires qbuem-json
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target middleware_example
./build/examples/11-advanced-apps/middleware/middleware_example
# Server starts on :8080

# Test CORS preflight
curl -X OPTIONS http://localhost:8080/api/data \
  -H "Origin: https://app.example.com" \
  -H "Access-Control-Request-Method: GET" -v

# Test rate limit (run quickly)
for i in $(seq 1 20); do curl -s http://localhost:8080/api/data; done
# After N requests: HTTP/1.1 429 Too Many Requests

# Test SSE
curl http://localhost:8080/events
```

## Notes

- Middleware is evaluated in the order it is registered with `app.use()`.
- Rate limits are per-IP; use `RateLimitMiddleware::with_key_fn()` for custom keying (e.g., per user_id).
- SSE connections stay open indefinitely; the server pushes events via `MessageBus` subscription.
- Requires `qbuem-json` for JSON event serialization in SSE payloads.
