# security_middleware

**Category:** Codec & Security
**File:** `security_middleware_example.cpp`
**Complexity:** Intermediate

## Overview

Demonstrates qbuem-stack's built-in security middleware components: `SecurityMiddleware` (sets HTTP security headers), `TokenAuthMiddleware` (Bearer token validation), `StaticFilesMiddleware` (safe static file serving), and `BodyEncoderMiddleware` (response body transformation).

## Scenario

A REST API server needs to enforce security best practices: reject unauthorized requests before they hit business logic, add OWASP-recommended HTTP security headers to every response, and serve static assets safely.

## Architecture Diagram

```
  HTTP Request
       │
       ▼
  ┌─────────────────────────────────────────────────────┐
  │  Middleware Chain (ordered)                         │
  │                                                     │
  │  1. SecurityMiddleware                              │
  │     ├─ X-Content-Type-Options: nosniff             │
  │     ├─ X-Frame-Options: DENY                       │
  │     ├─ X-XSS-Protection: 1; mode=block             │
  │     ├─ Strict-Transport-Security: max-age=...      │
  │     └─ Content-Security-Policy: default-src 'self' │
  │                                                     │
  │  2. TokenAuthMiddleware                             │
  │     ├─ Extract "Authorization: Bearer <token>"     │
  │     ├─ Compare token (constant-time)               │
  │     └─ Return 401 if invalid                       │
  │                                                     │
  │  3. BodyEncoderMiddleware                           │
  │     └─ Transforms response body (e.g. compress)    │
  │                                                     │
  │  4. Route Handler                                   │
  │     └─ Business logic                              │
  └─────────────────────────────────────────────────────┘
       │
       ▼
  HTTP Response (with security headers)

  StaticFilesMiddleware (separate route)
  ──────────────────────────────────────
  GET /static/foo.html
       │
       ▼  path sanitization (no ../ traversal)
  serve_file(root_dir + path)
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `SecurityMiddleware` | Adds OWASP security headers |
| `TokenAuthMiddleware(token)` | Bearer token authentication |
| `StaticFilesMiddleware(root_dir)` | Safe static file serving |
| `BodyEncoderMiddleware` | Response body transformation |
| `app.use(middleware)` | Register middleware in the chain |

## Input / Output

| Request | Auth Token | Response |
|---------|-----------|----------|
| `GET /api/data` with valid token | Matches configured token | `200 OK` + security headers |
| `GET /api/data` with wrong token | Mismatch | `401 Unauthorized` |
| `GET /static/index.html` | None required | File content or `404` |

## Security Headers Added

| Header | Value |
|--------|-------|
| `X-Content-Type-Options` | `nosniff` |
| `X-Frame-Options` | `DENY` |
| `X-XSS-Protection` | `1; mode=block` |
| `Strict-Transport-Security` | `max-age=31536000; includeSubDomains` |
| `Content-Security-Policy` | `default-src 'self'` |

## How to Run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target security_middleware_example
./build/examples/04-codec-security/security_middleware/security_middleware_example
```

## Notes

- `TokenAuthMiddleware` uses `constant_time_equal` internally to prevent timing attacks.
- `StaticFilesMiddleware` rejects paths containing `../` to prevent directory traversal.
- For JWT-based authentication, see `JwtAuthAction` in the pipeline layer.
