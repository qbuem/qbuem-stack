# coro_json

**Category:** Database / Integration
**File:** `coro_json.cpp`
**Complexity:** Intermediate
**Dependencies:** `qbuem-json`

## Overview

Demonstrates coroutine-based JSON serialization and deserialization integrated with qbuem-stack's async HTTP handlers using `qbuem-json`. Shows how to parse request bodies and serialize response objects without blocking the event loop.

## Scenario

A REST API endpoint receives JSON payloads, deserializes them in a coroutine handler, processes the data, and serializes the response — all non-blocking.

## Architecture Diagram

```
  POST /api/orders
  Content-Type: application/json
  {"order_id": 1001, "symbol": "AAPL", "qty": 10}
       │
       ▼
  AsyncHandler (coroutine)
  ├─ co_await req.json<OrderRequest>()  → OrderRequest struct
  ├─ validate(order)
  ├─ process(order)                     → OrderResponse struct
  └─ res.json(response)                 → serialized JSON body
       │
       ▼
  HTTP/1.1 200 OK
  Content-Type: application/json
  {"order_id": 1001, "status": "accepted", "timestamp": ...}
```

## Key APIs Used

| API | Purpose |
|-----|---------|
| `req.json<T>()` | Deserialize request body to struct `T` |
| `res.json(obj)` | Serialize `obj` to JSON response body |
| `qbuem_json::to_json(obj)` | Manual serialization |
| `qbuem_json::from_json<T>(str)` | Manual deserialization |

## Input

HTTP POST body:
```json
{"order_id": 1001, "symbol": "AAPL", "quantity": 10, "price": 175.50}
```

## Output

```json
{"order_id": 1001, "status": "accepted", "filled_qty": 10, "avg_price": 175.50}
```

## How to Run

```bash
# Requires qbuem-json
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target coro_json
./build/examples/09-database/coro_json/coro_json
```

## Notes

- `qbuem-json` has zero external dependencies — it is a header-only library bundled with qbuem-stack's optional components.
- JSON parsing errors produce a `Result` with an appropriate error code; always handle the error case.
- For binary serialization (protobuf, flatbuffers), use a `BodyEncoderMiddleware` instead.
