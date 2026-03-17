# trading_platform

**Category:** Advanced Applications
**File:** `trading_platform.cpp`
**Complexity:** Expert
**Dependencies:** `qbuem-json`

## Overview

A complete high-frequency trading platform built on qbuem-stack. Combines a WAS HTTP server, `StaticPipeline` (order processing with auto-scale and SLO tracking), `DynamicPipeline` (strategy hot-swap), `MessageBus` (SSE fan-out to clients), mock DB, JSON serialization, Context propagation, and cross-reactor response channels.

## Scenario

A trading platform receives order requests via REST API. Orders flow through a 3-stage pipeline (validate → risk-check → route-to-exchange), with auto-scaling under load and SLO tracking. Market data updates are broadcast via Server-Sent Events. Strategy parameters can be hot-swapped without restarting.

## Architecture Diagram

```
  ┌──────────────────────────────────────────────────────────────┐
  │  qbuem-stack Trading Platform                               │
  │                                                              │
  │  REST API (WAS)                                             │
  │  ┌────────────────────────────────────────────────────────┐  │
  │  │  POST /api/v1/orders   → submit order                  │  │
  │  │  GET  /api/v1/orders/:id → order status               │  │
  │  │  POST /api/v1/strategy → hot-swap strategy             │  │
  │  │  GET  /events          → SSE market data stream        │  │
  │  └───────────────────────────┬────────────────────────────┘  │
  │                              │  OrderRequest (JSON)           │
  │                              ▼                                │
  │  ┌────────────────────────────────────────────────────────┐  │
  │  │  StaticPipeline (auto-scale, SLO)                      │  │
  │  │  OrderRequest → ValidOrder → RiskResult → RouteResult  │  │
  │  │                                                        │  │
  │  │  [validate]─►[risk_check]─►[route_to_exchange]        │  │
  │  │   concurrency=2   concurrency=4  concurrency=2         │  │
  │  │   auto_scale=[2,8]                                     │  │
  │  │   SLO: p99 < 10ms                                     │  │
  │  └───────────────────────────┬────────────────────────────┘  │
  │                              │                                │
  │  ┌────────────────────────────────────────────────────────┐  │
  │  │  DynamicPipeline (strategy hot-swap)                   │  │
  │  │  RouteResult → FillResult                              │  │
  │  │  [market_order] ←hot_swap→ [limit_order]               │  │
  │  └───────────────────────────┬────────────────────────────┘  │
  │                              │                                │
  │        ┌─────────────────────┤                               │
  │        │                     │                               │
  │        ▼                     ▼                               │
  │  MockDB (fills)    MessageBus ("market_data")                │
  │                    │                                         │
  │                    ▼  SSE fan-out                            │
  │               N SSE clients  (/events)                       │
  └──────────────────────────────────────────────────────────────┘
```

## Key Features Demonstrated

| Feature | Implementation |
|---------|---------------|
| REST API | `App::get/post` with coroutine handlers |
| JSON I/O | `qbuem-json` req/res serialization |
| Auto-scale | `StaticPipeline` concurrency auto-scaling |
| SLO Tracking | `LatencyHistogram` + `ErrorBudgetTracker` |
| Hot-swap | `DynamicPipeline::hot_swap` for strategy change |
| SSE | `MessageBus` → `ServerSentEvents` middleware |
| Context propagation | `Context` carries trace-id across stages |
| Cross-reactor response | `co_await` response channel from pipeline back to HTTP |

## API Endpoints

| Method | Path | Description |
|--------|------|-------------|
| `POST` | `/api/v1/orders` | Submit a new order |
| `GET` | `/api/v1/orders/:id` | Get order status |
| `POST` | `/api/v1/strategy` | Hot-swap strategy (`market`/`limit`) |
| `GET` | `/api/v1/metrics` | Get pipeline SLO metrics (JSON) |
| `GET` | `/events` | SSE stream for real-time market data |

## Input

```json
POST /api/v1/orders
{
  "symbol": "SAMSUNG",
  "side": "BUY",
  "quantity": 100,
  "price": 72500.0,
  "type": "LIMIT"
}
```

## Output

```json
{
  "order_id": "ORD-00001",
  "status": "ACCEPTED",
  "filled_qty": 100,
  "avg_price": 72500.0,
  "latency_us": 312
}
```

## How to Run

```bash
# Requires qbuem-json
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target trading_platform
./build/examples/11-advanced-apps/trading_platform/trading_platform
# Server starts on :8090

# Test
curl -X POST http://localhost:8090/api/v1/orders \
  -H 'Content-Type: application/json' \
  -d '{"symbol":"SAMSUNG","side":"BUY","quantity":100,"price":72500}'

# SSE stream
curl http://localhost:8090/events
```

## Notes

- This example is the most comprehensive in the repository — ~60 KB of code demonstrating the full qbuem-stack feature set.
- Auto-scale: when p99 latency exceeds the SLO threshold, the pipeline automatically spawns additional action instances.
- The mock DB uses `InMemorySessionStore` for order storage.
