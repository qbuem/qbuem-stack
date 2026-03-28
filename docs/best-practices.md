# qbuem-stack Best Practices — Pattern Selection Guide

This document explains **which pattern to choose** for each situation, and why.
Every section includes the decision criteria, the right API, and a minimal example.

---

## Table of Contents

1. [Async Core — Task, Dispatcher, Reactor](#1-async-core)
2. [Pipeline System — Static, Dynamic, Graph](#2-pipeline-system)
3. [Channels — SPSC, Async, Priority, Arena](#3-channels)
4. [IPC & Messaging — SHMChannel, MessageBus, SHMBus](#4-ipc-and-messaging)
5. [Network — TCP, UDP, Unix Sockets, UDS](#5-network)
6. [HTTP Client — fetch, FetchPipeline, fetch_stream](#6-http-client)
7. [HTTP Server — Router, Middleware, App](#7-http-server)
8. [IO Primitives — IOVec, scattered_span, writev](#8-io-primitives)
9. [Memory — Arena, FixedPoolResource, LockFreeHashMap](#9-memory)
10. [Resilience — Retry, CircuitBreaker, DLQ, Saga](#10-resilience)
11. [Observability — Tracing, TimerWheel, TaskGroup](#11-observability)
12. [Crypto — Which primitive for what](#12-crypto)
13. [Middleware — Inbound HTTP middleware chain](#13-middleware)
14. [Hardware & Platform-Specific](#14-hardware-and-platform-specific)

---

## 1. Async Core

### Task\<T\> — the universal coroutine return type

```cpp
#include <qbuem/core/task.hpp>
```

**Always return `Task<T>` from coroutines.** Do not return `std::coroutine_handle` directly.

| Situation | Pattern |
|-----------|---------|
| CPU-only computation, no I/O | Regular function — do NOT wrap in `Task<>` |
| Any I/O (network, file, DB) | `Task<T>` with `co_await` |
| Fallible operations | `Task<Result<T>>` (`std::expected<T, std::error_code>`) |
| Void async operation | `Task<void>` |
| Fire-and-forget | `disp.spawn(my_task())` — returns immediately |

### Dispatcher — the event loop

```cpp
#include <qbuem/core/dispatcher.hpp>

Dispatcher disp;
disp.spawn(my_coroutine());
disp.run();   // blocks until all tasks complete
```

**One Dispatcher per thread.** Never share a Dispatcher across threads. For multi-core parallelism, run one Dispatcher per core and use channels to communicate.

### TaskGroup — structured concurrency

```cpp
#include <qbuem/pipeline/task_group.hpp>

TaskGroup group;
group.spawn(fetch_sensor_a());
group.spawn(fetch_sensor_b());
auto [a, b] = co_await group.join_all<SensorData>();
```

Use `TaskGroup` when:
- You need to fan out work and collect all results
- You want automatic cancellation: if one task fails, the group cancels remaining tasks

Do NOT use `TaskGroup` when:
- Tasks are independent and you want to ignore failures — use individual `spawn()` + channels
- You only need to wait for the first result — use `join_any()`

---

## 2. Pipeline System

### Which pipeline type to choose

| Need | Type | Header |
|------|------|--------|
| Fixed compile-time stage chain | `StaticPipeline` | `static_pipeline.hpp` |
| Stages need runtime hot-swap | `DynamicPipeline` | `dynamic_pipeline.hpp` |
| Fan-out / fan-in DAG topology | `PipelineGraph` | `pipeline_graph.hpp` |
| Each message routes differently | `DynamicRouter` | `dynamic_router.hpp` |
| Sub-pipeline per domain | `SubpipelineAction` | `subpipeline_action.hpp` |

### StaticPipeline — compile-time type safety

```cpp
#include <qbuem/pipeline/static_pipeline.hpp>

// Stage signature — always: Task<Result<Out>>(In, std::stop_token)
Task<Result<ParsedEvent>> parse(RawBytes raw, std::stop_token st) {
    if (st.stop_requested()) co_return std::unexpected(errc::operation_canceled);
    co_return ParsedEvent{...};
}

auto pipeline = PipelineBuilder<RawBytes, FinalResult>{}
    .add<ParsedEvent>(parse)
    .add<ValidatedEvent>(validate)
    .add<FinalResult>(transform)
    .build();

pipeline.start(dispatcher);
pipeline.push(raw_input);
auto result = co_await pipeline.pull();
pipeline.close();
```

Use when: stage types are known at compile time and will not change at runtime.

### DynamicPipeline — runtime hot-swap

```cpp
#include <qbuem/pipeline/dynamic_pipeline.hpp>

DynamicPipeline dp;
dp.add_stage("parse",    parse_fn);
dp.add_stage("validate", validate_fn);
dp.start(dispatcher);

// Replace a stage without downtime
dp.hot_swap("validate", new_validate_fn);
```

Use when: A/B testing, canary deployments, or runtime reconfiguration is required.

### PipelineGraph — DAG fan-out / fan-in

```cpp
#include <qbuem/pipeline/pipeline_graph.hpp>

PipelineGraph graph;
graph.add_node("ingest",   ingest_fn);
graph.add_node("branch_a", branch_a_fn);
graph.add_node("branch_b", branch_b_fn);
graph.add_node("merge",    merge_fn);

graph.connect("ingest",   {"branch_a", "branch_b"});  // fan-out
graph.connect({"branch_a", "branch_b"}, "merge");      // fan-in
graph.start(dispatcher);
```

Use when: multiple independent processing branches feed back into a single stage.

### Action patterns

| Pattern | Header | Use case |
|---------|--------|----------|
| `BatchAction<I,O>` | `batch_action.hpp` | Accumulate N items → dispatch to hardware/GPU in one call |
| `WindowedAction` | `windowed_action.hpp` | Tumbling / sliding / session windows with watermarks |
| `ScatterGatherAction` | `pipeline_graph.hpp` | Fan-out to N backends, wait for all, merge |
| `StatefulAction` | `stateful_window.hpp` | Per-key stateful aggregation (count, sum, last-N) |
| `IdempotencyFilter` | `idempotency.hpp` | Deduplicate by ID; skip already-processed messages |

---

## 3. Channels

### Which channel to choose

| Need | Channel | Header |
|------|---------|--------|
| Single producer, single consumer, hot path | `SpscChannel<T>` | `spsc_channel.hpp` |
| Multiple producers or consumers | `AsyncChannel<T>` | `async_channel.hpp` |
| Priority-based delivery | `PriorityChannel<T>` | `priority_channel.hpp` |
| Zero-copy: producer allocates from channel arena | `ArenaChannel<T>` | `arena_channel.hpp` |

### SpscChannel — fastest, zero-overhead

```cpp
#include <qbuem/pipeline/spsc_channel.hpp>

SpscChannel<Order> ch{1024};   // power-of-2 capacity

// Producer
ch.push(order);                // or: ch.try_push(order)

// Consumer
co_await ch.pull();            // or: ch.try_pull()
```

**Golden rule**: Only use `SpscChannel` when there is exactly ONE producer thread and ONE consumer coroutine. Any other use causes data races.

### AsyncChannel — general purpose

```cpp
#include <qbuem/pipeline/async_channel.hpp>

AsyncChannel<Event> ch{256};
ch.push(event);                   // producer (any thread/coroutine)
auto e = co_await ch.pull();      // consumer (coroutine)
ch.close();                       // signal EOF — pull returns std::nullopt
```

### PriorityChannel — deadline / urgency ordering

```cpp
#include <qbuem/pipeline/priority_channel.hpp>

PriorityChannel<Alert, int> ch{256};  // second template = priority type
ch.push(alert, /*priority=*/10);
ch.push(heartbeat, /*priority=*/1);
auto [item, pri] = co_await ch.pull(); // Alert delivered first
```

---

## 4. IPC and Messaging

### Which IPC mechanism to choose

| Situation | Mechanism | Header |
|-----------|-----------|--------|
| Same process, multiple components | `MessageBus` | `message_bus.hpp` |
| Cross-process, same machine (shared memory) | `SHMChannel<T>` | `shm_channel.hpp` |
| Cross-process pub/sub | `SHMBus` | `shm_bus.hpp` |
| Pass file descriptors + data over Unix socket | `uds::send_fds` | `uds_advanced.hpp` |

### MessageBus — in-process pub/sub

```cpp
#include <qbuem/pipeline/message_bus.hpp>

MessageBus bus;
auto sub = bus.subscribe<ValidatedOrder>("validated");

// Publisher (any coroutine)
bus.publish("validated", order);

// Subscriber
auto order = co_await sub->receive();
```

Use when: decoupling components within one process without shared state.

### SHMChannel — cross-process zero-copy

```cpp
#include <qbuem/shm/shm_channel.hpp>

// Producer process
SHMChannel<MarketTick> ch("market_feed", SHMChannel<MarketTick>::Mode::Producer);
ch.push(tick);

// Consumer process
SHMChannel<MarketTick> ch("market_feed", SHMChannel<MarketTick>::Mode::Consumer);
auto tick = co_await ch.pull();
```

**Requirement**: `T` must satisfy `std::is_trivially_copyable_v<T>`. No `std::string`, `std::vector`, or any type with a destructor.

### SHMBus — cross-process topic pub/sub

```cpp
#include <qbuem/shm/shm_bus.hpp>

SHMBus bus("trading_bus", SHMBus::Scope::LOCAL_ONLY);

// Publisher
bus.publish("orders", order);

// Subscriber
auto sub = bus.subscribe<Order>("orders");
auto o = co_await sub->receive();
```

Use `SHMBus` when multiple processes need a decoupled pub/sub topology. Use `SHMChannel` when exactly one producer talks to one consumer for maximum throughput.

---

## 5. Network

### Which socket type to choose

| Use case | Socket type | Header |
|----------|------------|--------|
| HTTP, WebSocket, generic TCP | `TcpStream` / `TcpListener` | `tcp_stream.hpp`, `tcp_listener.hpp` |
| Low-latency telemetry, media | `UdpSocket` | `udp_socket.hpp` |
| High-throughput UDP batching (Linux) | `udp_mmsg` (recvmmsg/sendmmsg) | `udp_mmsg.hpp` |
| Same-machine IPC | `UnixSocket` | `unix_socket.hpp` |
| IPC + file descriptor passing | `uds::send_fds` | `uds_advanced.hpp` |
| Multicast groups | `UdpMulticast` | `udp_multicast.hpp` |
| Reliable UDP (RUDP) | `RudpSocket` | `rudp_socket.hpp` |

### TCP patterns

```cpp
#include <qbuem/net/tcp_listener.hpp>

TcpListener listener;
co_await listener.bind("0.0.0.0", 8080);

while (true) {
    auto stream = co_await listener.accept();
    disp.spawn(handle_client(std::move(stream)));
}
```

### UDP — choose based on message rate

```cpp
// Low rate (< ~100k msg/s): UdpSocket
UdpSocket sock;
co_await sock.bind("0.0.0.0", 9000);
auto [data, from] = co_await sock.recv_from(buf);

// High rate (Linux only): recvmmsg — receive up to 32 datagrams in one syscall
#include <qbuem/net/udp_mmsg.hpp>
UdpMmsg sock;
auto batch = co_await sock.recv_batch(32);  // up to 32 msgs per syscall
```

---

## 6. HTTP Client

### Which HTTP client to use

| Need | API | Header |
|------|-----|--------|
| Simple one-off HTTP request | `qbuem::fetch()` | `fetch.hpp` |
| With retry / circuit-breaker | `FetchPipeline` | `fetch_pipeline.hpp` |
| Large response body (streaming) | `fetch_stream()` | `fetch_stream.hpp` |
| HTTPS (TLS) | `fetch_tls()` | `fetch_tls.hpp` |
| HTTP/2 (multiplexing) | `Http2Client` | `http2_client.hpp` |
| gRPC | `GrpcChannel` | `grpc/grpc_channel.hpp` |

### FetchPipeline — recommended for production

```cpp
#include <qbuem/http/fetch_pipeline.hpp>
#include <qbuem/http/backoff.hpp>

auto pipe = FetchPipeline{}
    .use(stages::logging("my-api"))
    .use(stages::retry_jitter(3))            // 3 attempts, jittered backoff
    .use(stages::circuit_breaker());         // open after 5 failures

auto resp = co_await pipe.execute({
    .method = "POST",
    .url    = "https://api.example.com/data",
    .body   = json_payload,
});
if (!resp) { /* circuit open or all retries exhausted */ }
```

### Backoff strategy selection

| Situation | Strategy | Example |
|-----------|----------|---------|
| Stable internal service | `backoff::fixed(500ms)` | Health checks |
| External public API | `backoff::jitter(500ms, 30s)` | Avoids thundering herd |
| Critical retry path | `backoff::exponential(100ms, 60s)` | Payment processors |
| Bursty upstream | `backoff::decorrelated(500ms, 30s)` | Cloud APIs under load |

---

## 7. HTTP Server

### Router — registering handlers

```cpp
#include <qbuem/http/router.hpp>

auto router = std::make_shared<Router>();

// Sync handler (no I/O)
router->add_route(Method::Get, "/health",
    [](const Request& req, Response& res) {
        res.status(200).body(R"({"status":"ok"})");
    });

// Async handler (DB query, external call)
router->add_route(Method::Post, "/users/:id/data",
    [pool](const Request& req, Response& res) -> Task<void> {
        auto id = req.param("id");
        auto conn_r = co_await pool->acquire();
        // ...
        co_return;
    });
```

### Middleware — apply before route registration

```cpp
#include <qbuem/middleware/cors.hpp>
#include <qbuem/middleware/jwt.hpp>
#include <qbuem/middleware/rate_limit.hpp>
#include <qbuem/middleware/request_id.hpp>

// Sync middleware — return false to halt the chain
router->use([](const Request& req, Response& res) -> bool {
    res.header("X-Request-Id", generate_id());
    return true;
});

// Async middleware — omit next() call to halt
router->use_async([](const Request& req, Response& res,
                     std::function<Task<void>()> next) -> Task<void> {
    if (!validate_jwt(req)) {
        res.status(401).body(R"({"error":"unauthorized"})");
        co_return;
    }
    co_await next();
});
```

### Middleware stack ordering

Always apply middleware in this order:

```
1. request_id      — inject X-Request-Id before any logging
2. cors            — allow/deny cross-origin before auth
3. rate_limit      — reject before auth (cheap rejection)
4. jwt / token_auth — authenticate
5. content_type    — validate body format (only on write routes)
6. your handler
```

### Response helpers

```cpp
// SSE (Server-Sent Events) — streaming responses
#include <qbuem/middleware/sse.hpp>
auto sse = SSEWriter{res};
co_await sse.write("data: hello\n\n");

// Template rendering
#include <qbuem/http/template_engine.hpp>
auto html = TemplateEngine::render("index.html", ctx);
res.status(200).header("Content-Type", "text/html").body(html);

// Static files
#include <qbuem/middleware/static_files.hpp>
router->use(static_files("/public", "./www"));
```

---

## 8. IO Primitives

### IOVec\<N\> and scattered_span — zero-copy scatter-gather

```cpp
#include <qbuem/io/iovec.hpp>
#include <qbuem/io/scattered_span.hpp>
```

**Golden rule**: Use `IOVec<N>` + `scattered_span` whenever you would otherwise concatenate buffers before a `write()` call.

| Situation | Wrong | Right |
|-----------|-------|-------|
| Send header + body | `std::string buf = header + body; write(fd, buf)` | `IOVec<2>{header, body} → writev` |
| Send frame + payload | Allocate gathered buffer | `IOVec<2>` → `writev` or `sendmsg` |
| Pass FD + multi-segment data | `write()` then `sendmsg()` separately | `uds::send_fds(fd, fds, scattered_span)` |

```cpp
// One writev syscall — zero allocation, zero copy
IOVec<3> vec;
vec.push(status_line.data(),  status_line.size());
vec.push(headers.data(),      headers.size());
vec.push(body.data(),         body.size());

scattered_span scatter{vec};
::writev(fd, scatter.iov_data(), scatter.iov_count());
```

**Lifetime rule**: `scattered_span` does NOT own the iovec array. `IOVec<N>` must outlive the `scattered_span`.

### When to use which IO type

| Need | API | Header |
|------|-----|--------|
| Scatter-gather `write`/`writev` | `IOVec<N>` + `scattered_span` | `iovec.hpp`, `scattered_span.hpp` |
| Zero-copy file sendfile | `ZeroCopyFile` | `zero_copy.hpp` |
| Direct I/O (O_DIRECT) | `DirectFile` | `direct_file.hpp` |
| Async file I/O (`io_uring`) | `AsyncFile` | `async_file.hpp` |
| io_uring raw ops | `uring_ops` | `uring_ops.hpp` |
| Read buffer management | `ReadBuf` | `read_buf.hpp` |
| Write buffer pool | `BufferPool` | `buffer_pool.hpp` |
| kTLS (TLS in kernel) | `kTLS` | `ktls.hpp` |

---

## 9. Memory

### Which allocator to choose

| Need | Allocator | Header |
|------|-----------|--------|
| Per-request scratch space | `Arena` | `reactor/arena.hpp` |
| Large aligned anonymous mmap | `MmapArena` | `reactor/mmap_arena.hpp` |
| Fixed-size object pool | `FixedPoolResource<T,N>` | `reactor/arena.hpp` |
| Concurrent key→value (hot path) | `LockFreeHashMap<K,V>` | `buf/lock_free_hash_map.hpp` |
| Linked list without allocation | `IntrusiveList<T>` | `buf/intrusive_list.hpp` |
| Object pool with generational safety | `GenerationPool<T>` | `buf/generation_pool.hpp` |
| NUMA-aware allocation | `numa::alloc()` | `reactor/numa.hpp` |
| 2MB hugepages | `HugePages::alloc()` | `reactor/huge_pages.hpp` |
| 2.5D spatial grid | `GridBitset<W,H,D>` | `buf/grid_bitset.hpp` |
| Infinite tiled spatial index | `TiledBitset` | `buf/tiled_bitset.hpp` |

### Arena — per-request lifetime

```cpp
#include <qbuem/reactor/arena.hpp>

Arena arena{65536};   // 64 KB slab

// Per request:
auto* ctx = arena.allocate<RequestContext>();
auto* buf = arena.allocate_array<char>(1024);

// End of request — O(1) reset, no destructor calls
arena.reset();
```

**Rule**: `Arena` objects allocated after the last `reset()` must NOT be used after the next `reset()`. Treat the arena as a stack frame.

### LockFreeHashMap — concurrent hot-path lookups

```cpp
#include <qbuem/buf/lock_free_hash_map.hpp>

LockFreeHashMap<uint64_t, Order> orders{4096};  // must be power of 2

orders.insert(order_id, order);
auto* o = orders.find(order_id);   // returns nullptr if not found
orders.erase(order_id);
```

Use when: multiple threads read/write the same map on the hot path.
Do NOT use `std::unordered_map` with a mutex on the hot path.

### GenerationPool — safe concurrent object pool

```cpp
#include <qbuem/buf/generation_pool.hpp>

GenerationPool<Event, 256> pool;

auto [token, ev] = pool.acquire();    // returns generation token + pointer
pool.release(token);                  // safe — stale pointers cannot access released slots
```

Use when: you need `shared_ptr`-like safety without `shared_ptr`'s atomic refcount overhead.

---

## 10. Resilience

### Which resilience pattern to use

| Failure scenario | Pattern | Header |
|------------------|---------|--------|
| Transient network errors | `RetryAction` with backoff | `retry_policy.hpp` |
| Downstream service failing continuously | `CircuitBreaker` | `circuit_breaker.hpp` |
| Unrecoverable / poison messages | `DeadLetterQueue` | `dead_letter.hpp` |
| Multi-step distributed transaction | `Saga` | `saga.hpp` |
| Progressive traffic shift | `CanaryRouter` | `canary.hpp` |
| Batch checkpoint + resume | `CheckpointedPipeline` | `checkpoint.hpp` |
| Deduplicate retried requests | `IdempotencyFilter` | `idempotency.hpp` |
| SLO tracking (p99, error budget) | `ErrorBudgetTracker` | `slo.hpp` |

### Retry + CircuitBreaker — the standard resilience triad

```cpp
#include <qbuem/pipeline/retry_policy.hpp>
#include <qbuem/pipeline/circuit_breaker.hpp>
#include <qbuem/pipeline/dead_letter.hpp>

CircuitBreaker cb{.failure_threshold = 5,
                  .recovery_timeout  = std::chrono::seconds{30}};
DeadLetterQueue<Order> dlq;

auto stage = RetryAction{backoff::jitter(100ms, 5s)}
    .with_circuit_breaker(cb)
    .with_dead_letter_queue(dlq);
```

**When to use each component**:

- **Retry alone**: For idempotent operations with transient errors (DNS flaps, brief overload).
- **CircuitBreaker**: When retrying would make a struggling service worse (cascade prevention).
- **DLQ**: When some messages will never succeed — store them for offline reprocessing.
- **All three together**: For any external payment, API call, or database write in production.

### Saga — distributed transactions

```cpp
#include <qbuem/pipeline/saga.hpp>

SagaOrchestrator saga;
saga.add_step("reserve_inventory",
    forward_fn,          // reserve stock
    compensate_fn);      // release stock on rollback
saga.add_step("charge_payment",
    charge_fn,
    refund_fn);
saga.add_step("create_shipment",
    ship_fn,
    cancel_ship_fn);

co_await saga.execute(order_context);  // auto-rollback on any step failure
```

Use `Saga` when: multiple services must all succeed or all roll back, and you cannot use a distributed 2PC transaction.

### CanaryRouter — progressive deployment

```cpp
#include <qbuem/pipeline/canary.hpp>

CanaryRouter router{
    .stable_weight = 95,    // 95% to stable handler
    .canary_weight = 5,     // 5% to new version
};
router.set_stable(stable_handler);
router.set_canary(canary_handler);

// Shift traffic based on error rate
router.set_canary_weight(50);  // promote to 50% when metrics look good
```

---

## 11. Observability

### Tracing — W3C TraceContext

```cpp
#include <qbuem/tracing/trace_context.hpp>
#include <qbuem/tracing/span.hpp>
#include <qbuem/tracing/exporter.hpp>

auto ctx = TraceContext::from_headers(req.headers());
auto span = ctx.start_span("process_order");
span.set_attribute("order.id", order_id);
span.set_attribute("order.amount", amount);

// ... do work ...

span.end();         // records duration
ctx.inject_headers(resp.headers());  // propagate to downstream
```

### Samplers

```cpp
#include <qbuem/tracing/sampler.hpp>

// Sample 10% of traces
auto sampler = RatioSampler{0.10};

// Always trace errors
auto sampler = ErrorSampler{};

// Composite: sample 1% normally, 100% on error
auto sampler = CompositeSampler{RatioSampler{0.01}, ErrorSampler{}};
```

### TimerWheel — scheduled callbacks without allocations

```cpp
#include <qbuem/reactor/timer_wheel.hpp>

TimerWheel wheel{/*slots=*/1024, /*tick_ms=*/1};

// Schedule a callback 500ms from now
auto handle = wheel.schedule(500ms, [this] { flush_buffer(); });

// Cancel if needed
wheel.cancel(handle);

// Advance the wheel (call in your event loop tick)
wheel.tick();
```

Use `TimerWheel` for: keepalive timers, TTL expiry, retry scheduling, rate-limit window resets.
Do NOT use `std::this_thread::sleep_for()` or `co_await async_sleep()` in tight loops — use `TimerWheel` instead.

### LifecycleTracer — track component state transitions

```cpp
#include <qbuem/tracing/lifecycle_tracer.hpp>

LifecycleTracer tracer{"payment_service"};
tracer.transition(State::Idle, State::Processing, "order_received");
tracer.transition(State::Processing, State::Complete, "charge_succeeded");
```

### AsyncLogger — zero-allocation logging on the hot path

```cpp
#include <qbuem/reactor/async_logger.hpp>

// Hot path — enqueues a pre-formatted record without allocating
log.info("order_accepted", order_id);    // uses ring buffer

// Flush thread (runs separately) formats and writes to sink
log.flush();
```

**Rule**: Never call `std::format()` on the hot path. Format only in the flush thread.

---

## 12. Crypto

### Which primitive to use

| Need | API | Header |
|------|-----|--------|
| Authenticated symmetric encryption | `aes_gcm::encrypt()` | `crypto/aes_gcm.hpp` |
| High-throughput stream cipher | `chacha20_poly1305::seal()` | `crypto/chacha20_poly1305.hpp` |
| Key derivation from password | `pbkdf2::derive()` | `crypto/pbkdf2.hpp` |
| Key derivation from key material | `hkdf::expand()` | `crypto/hkdf.hpp` |
| HMAC message authentication | `hmac_sha256()` | `crypto/hmac.hpp` |
| Content hashing | `sha256()` / `sha512()` | `crypto/sha256.hpp` |
| Random key / token generation | `random_bytes<N>()` | `crypto/random.hpp` |
| JWT sign/verify | `simd_jwt::sign()` | `security/simd_jwt.hpp` |
| Constant-time comparison | `constant_time_equal()` | `crypto.hpp` |
| Base64URL encode/decode | `base64url_encode()` | `crypto/base64.hpp` |
| CSRF token | `csrf_token()` | `crypto.hpp` |
| Post-quantum crypto | `pqc::` | `security/pqc.hpp` |

### Decision rules

- **AES-GCM vs ChaCha20-Poly1305**: Both provide authenticated encryption. Use AES-GCM on hardware with AES-NI acceleration (x86/ARM with ARMv8-A Crypto Extensions). Use ChaCha20-Poly1305 on embedded / constrained hardware without AES acceleration.
- **PBKDF2 vs HKDF**: Use PBKDF2 for password hashing (slow by design). Use HKDF for deriving keys from existing key material (fast).
- **SHA-256 vs SHA-512**: SHA-256 for most uses. SHA-512 on 64-bit platforms for bulk hashing (two `uint64_t` words per step vs four `uint32_t`).

```cpp
#include <qbuem/crypto/aes_gcm.hpp>
#include <qbuem/crypto/random.hpp>

auto key   = random_bytes<32>();  // AES-256 key
auto nonce = random_bytes<12>();  // 96-bit nonce

auto ciphertext = aes_gcm::encrypt(key, nonce, plaintext, aad);
auto plaintext  = aes_gcm::decrypt(key, nonce, ciphertext, aad);
// decrypt returns std::expected — check before use
```

---

## 13. Middleware

### Standard middleware headers

| Middleware | Header | Purpose |
|-----------|--------|---------|
| CORS | `middleware/cors.hpp` | Cross-origin resource sharing |
| JWT auth | `middleware/jwt.hpp` | Validate Bearer token |
| Rate limiting | `middleware/rate_limit.hpp` | Token bucket, per-IP |
| Adaptive rate limiter | `middleware/adaptive_rate_limiter.hpp` | Auto-adjusts to load |
| Request ID | `middleware/request_id.hpp` | Inject X-Request-Id |
| Content-Type check | `middleware/content_type.hpp` | 415 on wrong Content-Type |
| Body encoding | `middleware/body_encoder.hpp` | gzip / deflate response |
| Compression | `middleware/compress.hpp` | Request decompression |
| Security headers | `middleware/security.hpp` | HSTS, CSP, X-Frame-Options |
| Token auth | `middleware/token_auth.hpp` | API key / Bearer token |
| Static files | `middleware/static_files.hpp` | Serve files from disk |
| SSE | `middleware/sse.hpp` | Server-Sent Events writer |
| Tracing | `http/trace_middleware.hpp` | Inject W3C trace context |

### Recommended middleware stack

```
Public routes:
  request_id → cors → rate_limit → [handler]

Authenticated routes:
  request_id → cors → rate_limit → jwt → [handler]

Write routes (POST/PUT/PATCH):
  request_id → cors → rate_limit → jwt → content_type(application/json) → [handler]

All routes (production):
  request_id → cors → rate_limit → security_headers → [auth] → [handler]
```

### require_json — validate Content-Type before touching body

```cpp
#include <qbuem/middleware/content_type.hpp>

router->use(qbuem::middleware::require_json());  // returns 415 if not application/json
router->add_route(Method::Post, "/users", create_user_handler);
```

---

## 14. Hardware and Platform-Specific

### Platform selection matrix

| Feature | Linux | macOS | Header |
|---------|-------|-------|--------|
| `io_uring` async I/O | ✓ | ✗ | `reactor/io_uring_reactor.hpp` |
| `kqueue` event loop | ✗ | ✓ | `reactor/kqueue_reactor.hpp` |
| `epoll` event loop | ✓ | ✗ | `reactor/epoll_reactor.hpp` |
| `recvmmsg`/`sendmmsg` batch UDP | ✓ | ✗ | `net/udp_mmsg.hpp` |
| NUMA-aware allocation | ✓ | partial | `reactor/numa.hpp` |
| Hugepages (2MB) | ✓ | ✗ | `reactor/huge_pages.hpp` |
| kTLS (kernel TLS offload) | ✓ | ✗ | `io/ktls.hpp` |
| `kqueue` timer/user event | ✗ | ✓ | `reactor/kqueue_reactor.hpp` |
| eBPF tracing | ✓ | ✗ | `ebpf/` |
| PCIe / RDMA / NVMe-oF | ✓ | ✗ | `pcie/`, `rdma/`, `spdk/` |

### MicroTicker — sub-millisecond precision timer

```cpp
#include <qbuem/reactor/micro_ticker.hpp>

// Drive a reactor at 1 µs precision (uses HPET / TSC)
MicroTicker ticker{std::chrono::microseconds{100}};
ticker.start([&reactor](auto now) {
    reactor.tick(now);
});
```

Use when: `TimerWheel` 1ms granularity is not enough — e.g., HFT matching engine heartbeats, hardware sensor polling.

### CPU affinity and cache hints

```cpp
#include <qbuem/reactor/cpu_hints.hpp>

// Bind this thread to core 3
cpu_hints::set_affinity(3);

// Prefetch a cache line
cpu_hints::prefetch(ptr);

// Pause hint (spin-wait backoff)
cpu_hints::pause();
```

### NUMA — allocate memory local to the processing core

```cpp
#include <qbuem/reactor/numa.hpp>

int node = numa::node_of_cpu(cpu_id);
void* buf = numa::alloc(size, node);   // allocates on node's local DRAM
numa::free(buf, size);
```

---

## Quick Reference — One-Page Pattern Map

```
REQUEST HANDLING
  sync, no I/O  → sync router handler (no Task<>)
  with DB/IO    → async Task<void> handler

PIPELINE TOPOLOGY
  A→B→C fixed   → StaticPipeline
  hot-swap      → DynamicPipeline
  fan-out/in    → PipelineGraph
  per-key state → StatefulAction
  time windows  → WindowedAction

DATA TRANSFER (same process)
  1P→1C fast   → SpscChannel
  N producers  → AsyncChannel
  pub/sub      → MessageBus
  priority     → PriorityChannel

DATA TRANSFER (cross process)
  1P→1C        → SHMChannel (trivially copyable T)
  pub/sub      → SHMBus
  FD + data    → uds::send_fds + scattered_span

NETWORK I/O
  HTTP/WS      → TcpListener + TcpStream
  low-rate UDP → UdpSocket
  high-rate UDP (Linux) → udp_mmsg (recvmmsg batch)
  IPC          → UnixSocket

HTTP CLIENT
  one-off      → qbuem::fetch()
  production   → FetchPipeline (retry + CB)
  streaming    → fetch_stream()

MEMORY
  per-request  → Arena (reset() at end)
  object pool  → FixedPoolResource<T,N>
  concurrent map → LockFreeHashMap<K,V>
  safe recycle → GenerationPool<T,N>
  NUMA         → numa::alloc(size, node)
  hugepages    → HugePages::alloc()

IO WRITE PATH
  multi-buffer → IOVec<N> + scattered_span → writev()
  zero-copy file → ZeroCopyFile / sendfile
  direct I/O   → DirectFile (O_DIRECT)
  async file   → AsyncFile (io_uring, Linux)

RESILIENCE
  transient errors   → RetryAction + backoff
  cascade prevention → CircuitBreaker
  poison messages    → DeadLetterQueue
  multi-step txn     → Saga
  progressive deploy → CanaryRouter
  deduplication      → IdempotencyFilter

CRYPTO
  encrypt/decrypt    → aes_gcm (AES-NI hw) or chacha20_poly1305
  password hash      → pbkdf2
  key derivation     → hkdf
  MAC                → hmac_sha256
  hash               → sha256 / sha512
  random             → random_bytes<N>()
  JWT                → simd_jwt

TIMING
  scheduled callbacks → TimerWheel
  sub-ms precision    → MicroTicker
  structured wait     → TaskGroup

OBSERVABILITY
  distributed tracing → TraceContext + Span + Exporter
  state tracking      → LifecycleTracer
  logging hot path    → AsyncLogger (enqueue only)
```
