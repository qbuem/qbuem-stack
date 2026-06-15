# HTTP & Web Server

This section documents the HTTP/Web layer of **qbuem-stack** — everything you need to
build an HTTP server, route requests, run middleware, render templates, talk to other
HTTP services with the curl-free fetch client, and serve WebSocket / HTTP/2 / gRPC traffic.

All types are POSIX-only (Linux `io_uring`, macOS/Jetson `kqueue`), zero-dependency
(C++23 stdlib + arch intrinsics), and errors are **returned, never thrown**:

```cpp
using Result<T> = std::expected<T, std::error_code>;   // from <qbuem/common.hpp>
// async results are Task<Result<T>>; check `if (!r) ... r.error()`.
```

> Namespace cheat sheet (verified against the headers — this matters because the layer
> spans two namespaces):
>
> | Symbol | Namespace | Header |
> |---|---|---|
> | `App`, `StackController`, `Metrics` | `qbuem` | `<qbuem/qbuem_stack.hpp>` |
> | `Request`, `Response`, `CookieOptions`, `Method` | `qbuem` | `<qbuem/http/request.hpp>`, `<qbuem/http/response.hpp>` |
> | `Router`, `RadixTree`, `Handler`, `AsyncHandler`, `Middleware`, `AsyncMiddleware`, `NextFn`, `ErrorHandler` | `qbuem` | `<qbuem/http/router.hpp>` |
> | `HttpParser` | `qbuem` | `<qbuem/http/parser.hpp>` |
> | `fetch`, `FetchRequest`, `FetchResponse`, `ParsedUrl` | `qbuem` | `<qbuem/http/fetch.hpp>` |
> | `FetchClient`, `ClientRequest` | `qbuem` | `<qbuem/http/fetch_client.hpp>` |
> | backoff factories, `RetryConfig`, `async_sleep` | `qbuem::backoff` | `<qbuem/http/backoff.hpp>` |
> | `TemplateEngine`, `CompiledTemplate`, `TemplateContext` | `qbuem::http` | `<qbuem/http/template_engine.hpp>` |
> | `make_trace_middleware`, `TraceMiddlewareConfig` | `qbuem::http` | `<qbuem/http/trace_middleware.hpp>` |
> | `Http1Handler`, `WebSocketHandler`, `Http2Handler`, `GrpcHandler`, `IConnectionHandler`, `AcceptLoop`, `ConnectionPool` | `qbuem` | `<qbuem/server/*.hpp>` |

Runnable examples live under `examples/02-network/` (`http_fetch/`, `websocket/`,
`http2_server/`, `grpc/`, `tcp_echo_server/`) and `examples/11-advanced-apps/middleware/`.

---

## 1. The `App` server — your main entry point

`App` is the high-level WAS (Web Application Server). It owns a `Dispatcher` (the
multi-reactor event loop) and a `Router`, accepts connections with `SO_REUSEPORT`, parses
HTTP/1.1 with `HttpParser`, dispatches to your route handlers, and writes the `Response`
back over the socket using a single `writev(2)` (header + body, zero gather copy).

**What it is** — the batteries-included HTTP/1.1 server. Construct it, register routes and
middleware, then call `listen()`.

**When to use it** — almost always, for serving HTTP. It handles keep-alive, 100-continue,
HEAD-from-GET, graceful drain, signal handling, static files, health probes, and metrics
for you.

**When NOT to** — if you need raw frame control (custom binary protocol, hand-rolled
HTTP/2, an embedded WebSocket-only endpoint) drop down to the `server/` handlers (§9-§12)
or to `TcpListener`/`TcpStream` directly (see `examples/02-network/tcp_echo_server/`).

### 1.1 Constructor & lifecycle

```cpp
#include <qbuem/qbuem_stack.hpp>
using namespace qbuem;

int main() {
    App app(4);                          // 4 reactor threads (default: hardware_concurrency)

    app.get("/hello", [](const Request& req, Response& res) {
        res.status(200)
           .header("Content-Type", "text/plain")
           .body("Hello, World!");
    });

    auto r = app.listen(8080);           // blocks until stop(); SIGTERM/SIGINT → graceful drain
    if (!r) {
        std::println(stderr, "listen failed: {}", r.error().message());
        return 1;
    }
    return 0;
}
```

| Member | Signature | Notes |
|---|---|---|
| ctor | `explicit App(size_t thread_count = std::thread::hardware_concurrency())` | One reactor per thread; connections are kernel-balanced via `SO_REUSEPORT`. |
| `listen` | `Result<void> listen(int port, bool ipv6 = false)` | Blocks. `ipv6=true` listens on IPv6 (dual-stack when `IPV6_V6ONLY=0`). Returns error on bind failure. |
| `listen_unix` | `Result<void> listen_unix(std::string_view path)` | AF_UNIX SOCK_STREAM. Socket file removed on graceful shutdown. Path ≲104 chars (OS limit). |
| `stop` | `void stop(int drain_timeout_ms = 5000)` | Async-signal-safe entry; sets drain flag, closes listen sockets, waits up to `drain_timeout_ms` for active connections, then stops reactors. `0` = stop immediately. |

`listen()` installs `SIGTERM`/`SIGINT` handlers that trigger a graceful drain — no new
connections are accepted and the process exits after the current poll cycle.

### 1.2 Routing methods

Each verb takes a path and a `HandlerVariant` (a `std::variant` of sync `Handler` or async
`AsyncHandler` — see §5). Because the route templates deduce sync vs async from the
callable's return type, you can pass a lambda directly.

| Method | Verb |
|---|---|
| `void get(std::string_view path, HandlerVariant)` | GET |
| `void post(std::string_view path, HandlerVariant)` | POST |
| `void put(std::string_view path, HandlerVariant)` | PUT |
| `void del(std::string_view path, HandlerVariant)` | DELETE (named `del` to avoid the `delete` keyword) |
| `void patch(std::string_view path, HandlerVariant)` | PATCH |
| `void head(std::string_view path, HandlerVariant)` | HEAD — if absent but a GET exists for the path, the server auto-serves HEAD from the GET handler and strips the body |
| `void options(std::string_view path, HandlerVariant)` | OPTIONS (CORS preflight, etc.) |

```cpp
app.post("/users", [](const Request& req, Response& res) {
    std::string_view body = req.body();              // raw bytes; parse JSON with your lib
    res.status(201)
       .header("Content-Type", "application/json")
       .body(R"({"created":true})");
});

// Path parameters with :name segments
app.get("/users/:id", [](const Request& req, Response& res) {
    std::string_view id = req.param("id");           // matched path parameter
    res.status(200).body(std::string("user ") + std::string(id));
});
```

### 1.3 Middleware, error handler

```cpp
app.use(my_sync_middleware);              // Middleware: bool(const Request&, Response&)
app.use_async(my_async_middleware);       // AsyncMiddleware: Task<bool>(Request&, Response&, NextFn)
app.on_error(my_error_handler);           // ErrorHandler: void(std::exception_ptr, const Request&, Response&)
```

A sync middleware returning `false` halts the chain; an async middleware must call
`co_await next()` to continue (see §6). `on_error` is called when a **sync** handler throws;
if unset, the server sends a generic 500. (Your own handler code should still prefer
returning error status codes over throwing — `Task<T>` coroutine bodies must not let
exceptions escape.)

See the full chain (CORS → rate-limit → request-id → HSTS → bearer-auth → routes) in
`examples/11-advanced-apps/middleware/middleware_example.cpp`.

### 1.4 Static file serving

```cpp
app.serve_static("/static", "./public");
// GET /static/js/app.js  →  reads ./public/js/app.js
```

`serve_static(url_prefix, root_dir)` registers a GET prefix route that:

- auto-detects MIME type from the file extension,
- emits a **weak ETag** (size + mtime) and `Last-Modified` → supports `304 Not Modified`,
- blocks path traversal (`/../`, `%2e%2e`, …).

On Linux the file body is sent with `sendfile(2)` (zero user-space copy) when the response
uses `Response::sendfile_path()` internally.

### 1.5 Health / readiness / liveness probes

| Method | Default path | Response |
|---|---|---|
| `health_check(path = "/health")` | `/health` | `200` + `{"status":"ok"}` |
| `health_check_detailed(path = "/health/detail")` | `/health/detail` | `{"status":"ok","connections":N,"uptime_s":T,"requests_total":N}` |
| `liveness_endpoint(path = "/live")` | `/live` | `200` while the process is alive (K8s liveness) |
| `readiness_endpoint(path = "/ready")` | `/ready` | `200` ready / `503` during drain (K8s readiness) |

```cpp
app.health_check();          // GET /health → {"status":"ok"}
app.readiness_endpoint();    // GET /ready  → 503 once stop() begins draining
app.liveness_endpoint();
```

The readiness probe flips to `503` the instant `stop()` is called, so a load balancer stops
routing new traffic before connections are torn down.

### 1.6 Access logging

| Method | Behaviour |
|---|---|
| `enable_access_log()` | One line/req to stderr: `[ISO8601] METHOD /path STATUS Xµs` |
| `enable_json_log()` | JSON/line: `{"ts":…,"method":…,"path":…,"status":N,"duration_us":N}` |
| `enable_structured_log()` | JSON/line incl. `remote_addr`, `request_id` (X-Request-Id), `trace_id` |
| `set_access_logger(fn)` | Custom callback: `void(string_view method, string_view path, int status, long duration_us)` |
| `set_structured_logger(fn)` | Custom callback taking `const App::StructuredLogRecord&` |

`StructuredLogRecord` fields: `method`, `path`, `status`, `duration_us`, `remote_addr`,
`request_id`, `trace_id`.

```cpp
app.set_access_logger([](std::string_view m, std::string_view p, int s, long us) {
    std::println(stderr, "{} {} {} {}us", m, p, s, us);
});
```

> **Gotcha:** the access-log callback runs **on the reactor thread** right after each
> response is sent. Keep it fast (or hand off to a ring buffer / `AsyncLogger`) — a slow
> logger directly adds latency to every request (Pillar 1, rule L7).

### 1.7 Connection limit & metrics

```cpp
app.set_max_connections(10'000);   // 0 = unlimited; over the limit → 503 + Retry-After: 1
app.metrics_endpoint();            // GET /metrics → Prometheus text exposition

Metrics m = app.snapshot_metrics();
// m.requests_total, m.errors_total, m.active_connections, m.bytes_sent
```

`metrics_endpoint(path = "/metrics")` exposes:

```
qbuem_requests_total N
qbuem_errors_total N
qbuem_active_connections N
qbuem_bytes_sent N
```

The four counters are `std::atomic<uint64_t>` placed on **separate 64-byte cache lines**
(`alignas(64)`) to avoid false sharing across reactor cores (Pillar 5, rule H4).
`snapshot_metrics()` is consistent per-field but not globally atomic across fields.

---

## 2. `StackController` — running multiple `App`s

**What it is** — a lifecycle controller that owns several `App` instances on different ports
and starts/stops them together with shared signal handling.

**When to use it** — when one process must serve more than one listener (e.g. a public API on
8080 and an internal admin API on 8081), or when you want one place to wire `SIGTERM`/`SIGINT`.

**When NOT to** — a single-port server: just call `app.listen(port)` directly.

```cpp
App api(4), admin(1);
// ... register routes on each ...

StackController ctrl;
ctrl.add(api,   8080);
ctrl.add(admin, 8081);
ctrl.run();      // blocks; each App listens in its own thread; joins all on shutdown
```

| Member | Signature |
|---|---|
| `add` | `void add(App& app, int port, bool ipv6 = false)` — the `App` must outlive `run()` |
| `run` | `void run()` — installs SIGTERM/SIGINT, blocks until all stop |
| `stop` | `void stop()` — request graceful shutdown of all apps (signal-safe) |

`StackController` is non-copyable. References are stored, so every `App` you `add()` must
remain alive for the duration of `run()`.

---

## 3. `Request` — the inbound value type

**What it is** — an immutable, **zero-copy** view of a parsed HTTP request. Header keys/values
and the body are `std::string_view`s into the connection receive buffer (which the server
keeps alive for the request's lifetime). Up to 32 headers and 16 path parameters are stored
inline in a flat array (zero heap allocation).

| Accessor | Returns | Notes |
|---|---|---|
| `method()` | `Method` | enum: `Get, Post, Put, Delete, Patch, Options, Head, Unknown` |
| `path()` | `std::string_view` | path **before** `?` |
| `query_string()` | `std::string_view` | everything after `?` |
| `body()` | `std::string_view` | raw bytes; parse JSON yourself |
| `remote_addr()` | `std::string_view` | immediate peer IP (may be a proxy/LB) |
| `header(key)` | `std::string_view` | O(n) linear scan over inline array (cache-friendly) |
| `param(key)` | `std::string_view` | `:name` path parameter |
| `query(key)` | `std::string_view` | URL query param; lazily parsed + cached on first call (values stay percent-encoded) |
| `cookie(key)` | `std::string_view` | from the `Cookie` header |
| `form(key)` | `std::string_view` | `application/x-www-form-urlencoded` body field; empty if Content-Type doesn't match |

```cpp
app.get("/search", [](const Request& req, Response& res) {
    std::string_view q   = req.query("q");                 // ?q=...
    std::string_view ua  = req.header("User-Agent");
    std::string_view sid = req.cookie("session");
    // For the originating client behind a proxy, prefer:
    std::string_view ip  = req.header("X-Forwarded-For");  // else req.remote_addr()
    res.status(200).body(std::string(q));
});
```

**Gotchas / constraints**
- The receive buffer **must outlive** the `Request` — never store a `string_view` from a
  `Request` past the handler invocation; copy into a `std::string` if you must keep it.
- `header()` lookup is **case-sensitive** (it compares the raw bytes). Use the exact header
  casing the client sent, or normalize upstream.
- `query()` has DoS guards: query strings > 64 KiB are ignored, and at most 128 params are
  parsed. `query()` returns the raw percent-encoded value — decode if needed.
- `remote_addr()` is set by the server from the socket (not from headers); for the real
  client behind a trusted proxy use `X-Forwarded-For` / `X-Real-IP`.

---

## 4. `Response` — the outbound builder

**What it is** — a chainable builder for status / headers / body, plus chunked transfer,
trailers, cookies, ETag/Last-Modified, and opt-in zero-copy `sendfile`.

| Method | Purpose |
|---|---|
| `Response& status(int)` | Set status code (default 200) |
| `Response& header(key, value)` | Set/overwrite a header |
| `Response& body(std::string_view)` | Set the body (copied internally) |
| `Response& chunk(std::string_view)` | Append a chunk (Transfer-Encoding: chunked) |
| `Response& end_chunks()` | Finalize chunked body (terminal 0-chunk) |
| `Response& trailer(key, value)` | Add a trailer field (chunked only) |
| `Response& set_cookie(name, value, const CookieOptions& = {})` | Append a `Set-Cookie` |
| `Response& etag(std::string_view)` | Sets `ETag` (auto-quoted unless already quoted / `W/`) |
| `Response& last_modified(std::time_t)` | RFC 7231 HTTP-date |
| `Response& sendfile_path(std::string_view path, size_t size)` | Zero-copy file send (Linux `sendfile`) |
| `std::string_view get_header(key) const` / `get_body() const` / `int status_code() const` | Read back |
| `std::string serialize() const` / `serialize_header() const` | Wire bytes (the server uses `serialize_header()` + writev) |

```cpp
app.get("/dl", [](const Request&, Response& res) {
    res.status(200)
       .header("Content-Type", "text/plain")
       .set_cookie("session", "abc123",
                   CookieOptions{.same_site = "Lax", .http_only = true, .secure = true})
       .etag("v2")                       // → ETag: "v2"
       .body("downloaded");
});

// Streaming (chunked) with a trailer:
app.get("/stream", [](const Request&, Response& res) {
    res.chunk("Hello, ").chunk("world!").end_chunks()
       .trailer("X-Checksum", "sha256=...");
});
```

`CookieOptions` fields: `path` (default `/`), `domain`, `same_site` (`"Strict"|"Lax"|"None"|""`),
`max_age` (`<0` = session cookie), `http_only`, `secure`.

**Gotchas**
- There is **no built-in JSON method** — serialize with your library of choice and pass the
  string to `body()`. (Pillar 4: JSON is an app-level concern, not in core headers.)
- `chunk()`/`end_chunks()` and `body()` are mutually exclusive on one response; chunked mode
  drops `Content-Length`.
- `sendfile_path()` is Linux-only; on macOS/Jetson set `body()` instead. Set `Content-Length`
  before calling if the size is known.

---

## 5. `Router` & `RadixTree` — route matching

**What it is** — the low-level routing engine `App` uses internally. A per-method radix tree
gives O(path-length) matching with binary search across child nodes once a node's branching
factor exceeds 4. You rarely touch `Router` directly when using `App`, but it's the type you
pass to the lower-level `Http1Handler` (§9).

Handler/middleware type aliases (all in `qbuem`):

```cpp
using Handler         = std::function<void(const Request&, Response&)>;
using AsyncHandler    = std::function<Task<void>(const Request&, Response&)>;
using HandlerVariant  = std::variant<std::monostate, Handler, AsyncHandler>;
using Middleware      = std::function<bool(const Request&, Response&)>;
using NextFn          = std::function<Task<void>()>;
using AsyncMiddleware = std::function<Task<bool>(const Request&, Response&, NextFn)>;
using ErrorHandler    = std::function<void(std::exception_ptr, const Request&, Response&)>;
```

`Router` API:

| Method | Purpose |
|---|---|
| `void add_route(Method, std::string_view path, HandlerVariant)` | Register a route (variant form) |
| `template<Fn> void add_route(Method, std::string_view path, Fn&&)` | Deduces sync vs async from `Fn`'s return type |
| `void use(Middleware)` / `void use_async(AsyncMiddleware)` | Append to the middleware chain |
| `HandlerVariant match(Method, std::string_view path, unordered_map<string,string>& params) const` | Match + fill path params |
| `void add_prefix_route(Method, std::string_view prefix, HandlerVariant)` | Prefix match; suffix exposed as `req.param("**")` |
| `bool path_exists(std::string_view path) const` | Distinguish 404 (unknown path) from 405 (method not allowed) |

```cpp
#include <qbuem/http/router.hpp>
using namespace qbuem;

Router router;
router.add_route(Method::Get, "/items/:id",
    [](const Request& req, Response& res) {
        res.status(200).body(std::string(req.param("id")));
    });

std::unordered_map<std::string, std::string> params;
auto hv = router.match(Method::Get, "/items/42", params);   // params["id"] == "42"
```

**Gotchas**
- Path parameters use the `:name` segment syntax (`/users/:id`). Wildcards are only available
  via `add_prefix_route` (suffix in `param("**")`), which is what `App::serve_static` uses.
- `match()` returning `std::monostate` means no handler; check `path_exists()` to choose
  between 404 and 405 (this is exactly what `Http1Handler`/`App` do).

---

## 6. Middleware patterns (sync & async)

**Sync middleware** runs before the handler; return `false` to short-circuit (the response
you've populated is sent as-is):

```cpp
app.use([](const Request& req, Response& res) -> bool {
    if (req.header("X-Api-Key") != "secret") {
        res.status(401).body("unauthorized");
        return false;            // stop the chain
    }
    return true;                 // continue
});
```

**Async (`next()`-based) middleware** wraps the rest of the chain — call `co_await next()` to
run downstream, then post-process; *not* calling `next()` stops the chain:

```cpp
app.use_async([](const Request& req, Response& res,
                 NextFn next) -> Task<bool> {
    auto t0 = std::chrono::steady_clock::now();
    co_await next();                                  // run handler + inner middleware
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    res.header("X-Elapsed-Us", std::to_string(us));  // post-processing
    co_return true;
});
```

**Gotchas**
- A `std::function`-typed middleware/handler is fine on the **cold** request-setup path, but
  do not allocate per-message inside the body on a high-throughput hot path (Pillar 3).
- Async middleware must `co_return` a `bool`. Forgetting `co_await next()` silently drops the
  rest of the chain — handy for auth gates, dangerous by accident.

---

## 7. Distributed tracing middleware (`make_trace_middleware`)

**What it is** — a factory (in `qbuem::http`) producing an `AsyncMiddleware` that implements
**W3C Trace Context Level 1**: it parses the inbound `traceparent` header, creates a child
span, records it in the global `PipelineTracer`, and (optionally) emits a `traceresponse`
header on the way out. It can also fall back to B3 (`X-B3-*`) headers.

**When to use it** — any service that participates in distributed tracing and must propagate
context to downstream calls.

```cpp
#include <qbuem/http/trace_middleware.hpp>

app.use_async(qbuem::http::make_trace_middleware());   // AlwaysSampler + traceresponse on

// With config:
app.use_async(qbuem::http::make_trace_middleware(qbuem::http::TraceMiddlewareConfig{
    .sampler          = nullptr,   // nullptr → AlwaysSampler
    .add_traceresponse = true,
    .parse_b3_fallback = true,     // also accept X-B3-TraceId/SpanId/Sampled
}));
```

`TraceMiddlewareConfig`: `sampler` (`std::shared_ptr<tracing::Sampler>`, default
`AlwaysSampler`), `add_traceresponse` (default `true`), `parse_b3_fallback` (default `false`).

Downstream, read the active context from the pipeline `Context` slot:

```cpp
if (auto slot = env.ctx.get<qbuem::TraceContextSlot>()) {
    auto child = slot->value.child_span();
    // child.to_traceparent() → set as the "traceparent" header on the outgoing fetch()
}
```

---

## 8. Template engine (`qbuem::http::TemplateEngine`)

**What it is** — a Mustache-like engine that **compiles a template once** into a segment IR
and renders it per-request with **zero heap allocation** for literal text (literals are
`string_view` slices of the owned source). `CompiledTemplate::render()` is `const` and
reentrant — multiple reactor threads can render the same template concurrently.

**When to use it** — server-rendered HTML/text/JSON fragments where the template is fixed at
startup and only the variables change per request.

**When NOT to** — large/complex view logic, or anything needing nested-block recursion beyond
`if`/`each` (the block parser is naive/non-nested for matching close tags); for binary or
streamed bodies use `Response::body()`/`chunk()` directly.

Syntax: `{{key}}` (HTML-escaped), `{{{key}}}` (raw), `{{#if key}}…{{/if}}`,
`{{#each key}}…{{/each}}` (`{{.}}` = current item), `{{! comment }}`, `{{> partial}}`.

```cpp
#include <qbuem/http/template_engine.hpp>
using qbuem::http::TemplateEngine;
using qbuem::http::TemplateContext;

TemplateEngine engine;                                  // create once at startup
engine.add_partial("header", "<header>{{title}}</header>");  // partials BEFORE compile()

auto tmpl = engine.compile(R"(
  {{> header}}
  <p>Hello, {{name}}!</p>
  {{#if admin}}<a href="/admin">Admin</a>{{/if}}
  <ul>{{#each items}}<li>{{.}}</li>{{/each}}</ul>
)");

// per request (zero allocation for literal segments):
TemplateContext ctx;
ctx.set("title", "My App");
ctx.set("name",  "Ada");
ctx.set("admin", "1");                                   // non-empty = truthy
ctx.set_list("items", {"a", "b", "c"});

std::string out;
out.reserve(4096);
tmpl.render(ctx, out);                                   // render into caller buffer
res.status(200).header("Content-Type", "text/html").body(std::move(out));
```

`TemplateContext`: `set(key, value)`, `set_list(key, vector<string>)`, `get(key)`,
`get_list(key)`, `is_truthy(key)` (non-empty = truthy), `clear()`.
`TemplateEngine`: `add_partial`, `compile`, `compile_cached(name, src)`, `get(name)`.
`CompiledTemplate`: `render(ctx, std::string&)`, `render(ctx) -> std::string`, `source()`,
`segment_count()`.

**Gotchas**
- Register partials **before** `compile()` — they're inlined at compile time.
- The source you compile is copied into the `CompiledTemplate` (owned), so the original
  string need not outlive it — but `TemplateContext` values are not copied; the strings you
  `set()` must stay alive for the duration of the `render()` call.
- Variables are HTML-escaped by default (`& < > " '`); use `{{{raw}}}` only for trusted HTML.

---

## 9. HTTP/1.1 connection handler (`Http1Handler`)

**What it is** — the lower-level, per-connection handler implementing
`IConnectionHandler<http::Request>` (§13). It runs the request loop for **one** connection:
detect WebSocket upgrade, send `100 Continue`, manage keep-alive, dispatch to a `Router`, and
write the response with one `writev` syscall (header + body, no gather copy — see
`examples/06-ipc-messaging/scatter_send/`).

**When to use it** — when you build your own accept loop or transport (custom listener, TLS
termination wrapper, embedding HTTP in another protocol) instead of `App::listen()`.

**When NOT to** — for an ordinary server, use `App` (§1); it already wires parsing + routing +
keep-alive + drain. `Http1Handler` is the build-your-own-server path.

```cpp
#include <qbuem/server/http1_handler.hpp>
using namespace qbuem;

auto router = std::make_shared<http::Router>();
router->add_route(Method::Get, "/hello",
    [](const Request& req, Response& res) { res.status(200).body("Hi"); });

// Optional WebSocket upgrade hook (see §10):
Http1Handler::UpgradeCallback on_upgrade =
    [](UpgradeRequest req) -> Task<void> { /* hand req.fd to a WebSocketHandler */ co_return; };

auto factory = [router, on_upgrade]{
    return std::make_unique<Http1Handler>(router, on_upgrade);
};
// Drive `factory` from your own accept loop and feed parsed http::Request frames to on_frame().
```

| Member | Signature | Notes |
|---|---|---|
| ctor | `Http1Handler(std::shared_ptr<http::Router> router, UpgradeCallback = nullptr)` | If `upgrade_callback` is null and a client sends `Upgrade: websocket`, the handler replies `426 Upgrade Required` |
| `Task<void> on_connect(int fd, SocketAddr remote)` | lifecycle | stores fd + addr, sets keep-alive on |
| `Task<Result<void>> on_frame(http::Request frame)` | per request | upgrade → 100-continue → keep-alive → router dispatch → writev response. Returning an error closes the connection |
| `Task<void> on_disconnect(std::error_code)` | lifecycle | resets fd; the accept loop owns `close()` |
| `bool keep_alive() const noexcept` | accessor | whether to reuse the connection |

`UpgradeRequest { http::Request original_request; int fd{-1}; }` is passed to your
`UpgradeCallback` when an `Upgrade: websocket` header is seen.

**Gotchas**
- This handler lives in `namespace qbuem` but its API uses the `http::Request` / `http::Router`
  spellings; include the request/router headers it pulls in and match that usage. For 99% of
  servers, prefer `App`, which uses `HttpParser` + `Router` directly and needs none of this.
- 404 vs 405 is resolved via `router->path_exists()` (path known, method not) → `405`.
- HEAD/`Expect: 100-continue` handling lives in `on_frame()`; if you write your own loop you
  feed it whole parsed requests.

---

## 10. WebSocket handler (`WebSocketHandler`) — RFC 6455

**What it is** — a server-side WebSocket implementation: the HTTP/1.1 upgrade handshake
(`101 Switching Protocols` with a `Sec-WebSocket-Accept` computed via an inline,
zero-dependency SHA-1 + Base64), frame encode/decode with SIMD XOR-masking (AVX2/SSE2 on
x86_64, NEON on AArch64/Jetson, scalar fallback), and automatic Ping/Pong/Close handling.

**When to use it** — real-time bidirectional channels (chat, live dashboards, game state)
hung off your HTTP server via `Http1Handler`'s upgrade callback.

**When NOT to** — request/response APIs (use routes), or server-push-only fan-out where SSE
(`<qbuem/middleware/sse.hpp>`, used in the middleware example) is simpler.

```cpp
#include <qbuem/server/websocket_handler.hpp>
using namespace qbuem;

auto ws = std::make_shared<WebSocketHandler>(
    [](WsFrame frame) -> Task<void> {
        if (frame.opcode == WsFrame::Opcode::Text) {
            std::string text(frame.payload.begin(), frame.payload.end());
            // ... application logic (echo, broadcast, etc.) ...
        }
        co_return;
    });

// Wire as the Http1Handler upgrade callback:
Http1Handler::UpgradeCallback on_upgrade =
    [ws](UpgradeRequest req) -> Task<void> {
        auto ok = co_await ws->upgrade(req.fd, req.original_request);
        if (ok) co_await ws->run(req.fd);     // frame receive/dispatch loop until close
    };
```

| Member | Signature | Notes |
|---|---|---|
| ctor | `WebSocketHandler(MessageHandler on_message)` | `MessageHandler = std::function<Task<void>(WsFrame)>`; control frames are handled internally, not delivered to you |
| `Task<Result<void>> upgrade(int fd, const Request& req)` | handshake | validates `Sec-WebSocket-Key`, sends `101`; `400` if key missing |
| `Task<void> run(int fd)` | loop | reads frames; Ping→Pong, Close→echo+exit, data→`on_message_` |
| `Task<Result<void>> send_text(int fd, std::string_view)` | send | UTF-8 text frame |
| `Task<Result<void>> send_binary(int fd, std::span<const uint8_t>)` | send | binary frame |
| `Task<Result<void>> send_close(int fd, uint16_t code = 1000)` | send | Close frame (status code) |
| `Task<Result<void>> send_ping(int fd)` | send | Ping frame |
| `static std::string compute_accept_key(std::string_view key)` | util | RFC 6455 §4.2.2 accept key |
| `static std::vector<uint8_t> encode_frame(const WsFrame&, bool mask = false)` | util | wire bytes (server→client: `mask=false`) |
| `static Result<WsFrame> decode_frame(std::span<const uint8_t>, size_t& consumed)` | util | partial data → `errc::resource_unavailable_try_again`, `consumed=0` |
| `static void xor_mask(const uint8_t* src, uint8_t* dst, size_t len, const std::array<uint8_t,4>& key)` | util | SIMD masking |

`WsFrame { Opcode opcode; bool fin; bool masked; std::vector<uint8_t> payload; }` where
`Opcode = {Continuation=0x0, Text=0x1, Binary=0x2, Close=0x8, Ping=0x9, Pong=0xA}`.

The static codec methods are demonstrated standalone (no socket needed) in
`examples/02-network/websocket/websocket_example.cpp`:

```cpp
std::string accept = WebSocketHandler::compute_accept_key("dGhlIHNhbXBsZSBub25jZQ==");
// → "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="

WsFrame f; f.opcode = WsFrame::Opcode::Text; f.fin = true;
std::string p = "Hello, WebSocket!";
f.payload.assign(reinterpret_cast<const uint8_t*>(p.data()),
                 reinterpret_cast<const uint8_t*>(p.data()) + p.size());
auto bytes = WebSocketHandler::encode_frame(f);

size_t consumed = 0;
auto decoded = WebSocketHandler::decode_frame({bytes.data(), bytes.size()}, consumed);
if (decoded) { /* use decoded->payload */ }
```

**Gotchas**
- `decode_frame()` returning an error with `errc::resource_unavailable_try_again` means *more
  bytes needed*, not a fatal error — keep buffering and retry. Frames > 16 MiB are rejected
  (`errc::message_size`) before allocation (DoS guard).
- Client→server frames must be masked (RFC 6455 §5.3); `encode_frame(frame, /*mask=*/true)`
  uses a **fixed** masking key (fine for tests/server-side; supply a random key path for true
  client emulation).
- The inline SHA-1 is for the handshake only — it is **not** a cryptographic primitive for
  security use.
- `run()`'s read loop uses a blocking `::read()` on the raw fd; it's intended to be driven on
  a worker spawned from the upgrade callback, not on a shared reactor thread doing other work.

---

## 11. HTTP/2 handler (`Http2Handler`) — minimal RFC 7540

**What it is** — a header-only HTTP/2 frame engine: frame parse/serialize (DATA, HEADERS,
SETTINGS, PING, GOAWAY, RST_STREAM, …), an HPACK decoder/encoder (static table + literal
encoding), and a per-stream state machine. You supply the socket I/O; the handler produces
frames to send via `drain_pending_frames()`.

**When to use it** — when you need server-side HTTP/2 framing (e.g. as the substrate for gRPC,
§12) and you control the transport.

**When NOT to** — plain HTTP serving (use `App`/HTTP/1.1). This is explicitly a **minimal**
implementation.

> **Documented limitations (from the header):** no HPACK *dynamic* table (static table 62
> entries + literal only); PRIORITY and PUSH_PROMISE are ignored on receipt; **flow control
> is not implemented** (WINDOW_UPDATE ignored). Treat it as a building block, not a
> production-grade HTTP/2 stack.

```cpp
#include <qbuem/server/http2_handler.hpp>
using namespace qbuem;

Http2Handler h2(
    [&h2](std::unordered_map<std::string, std::string> headers,
          std::vector<uint8_t> body,
          std::shared_ptr<Http2Stream> stream) -> Task<void> {
        co_await h2.send_headers(stream->id,
            {{":status", "200"}, {"content-type", "text/plain"}});
        co_await h2.send_data(stream->id, std::span<const uint8_t>(body), /*end_stream=*/true);
        co_return;
    });

co_await h2.send_connection_preface();
// network loop (you provide read_frame/write_frame):
for (;;) {
    Http2Frame frame = co_await read_frame(socket);
    co_await h2.handle_frame(std::move(frame));
    for (auto& f : h2.drain_pending_frames())
        co_await write_frame(socket, Http2Handler::serialize_frame(f));
}
```

Key public surface:

| Member | Signature |
|---|---|
| ctor | `Http2Handler(RequestHandler)` where `RequestHandler = std::function<Task<void>(unordered_map<string,string> headers, vector<uint8_t> body, shared_ptr<Http2Stream> stream)>` |
| `Task<Result<void>> handle_frame(Http2Frame)` | dispatch a received frame |
| `Task<Result<void>> send_headers(uint32_t stream_id, const unordered_map<string,string>& headers, bool end_stream = false)` | queue a HEADERS frame |
| `Task<Result<void>> send_data(uint32_t stream_id, std::span<const uint8_t> data, bool end_stream = true)` | queue a DATA frame |
| `Task<void> send_settings(bool ack = false)` / `send_ping(bool ack, uint64_t = 0)` / `send_goaway(...)` | connection frames |
| `Task<Result<void>> send_connection_preface()` | server preface + initial SETTINGS |
| `std::vector<Http2Frame> drain_pending_frames()` | take queued frames (buffer cleared) |
| `static std::vector<uint8_t> serialize_frame(const Http2Frame&)` | 9-byte header + payload to wire bytes |

`Http2Frame { uint32_t length; Http2FrameType type; uint8_t flags; uint32_t stream_id;
std::vector<uint8_t> payload; }`; `Http2FrameType { DATA=0x0, HEADERS=0x1, …, CONTINUATION=0x9 }`;
flag constants `HTTP2_FLAG_END_STREAM/END_HEADERS/ACK/PADDED/PRIORITY`.
`Http2Stream` carries `id`, `state` (`IDLE/OPEN/HALF_CLOSED_LOCAL/HALF_CLOSED_REMOTE/CLOSED`),
`request_headers`, `request_body`, and an outgoing `AsyncChannel<Http2Frame>`.

See `examples/02-network/http2_server/http2_server_example.cpp` for a frame round-trip walk.

**Gotcha** — because flow control is unimplemented, only use this for controlled / internal
traffic or as gRPC plumbing; do not expose it to arbitrary public HTTP/2 clients.

---

## 12. gRPC handler (`GrpcHandler<Req, Res>`)

**What it is** — a protobuf-**independent** gRPC method handler. It implements the gRPC
length-prefixed message framing (`[compressed:1][len:4 BE][payload]`) and the four streaming
patterns, while delegating serialization to **caller-supplied** functions — so you can use
protobuf, FlatBuffers, or anything else with zero IDL dependency in core.

**When to use it** — exposing gRPC endpoints from a qbuem service (paired with `Http2Handler`
§11 for transport), with your own codec.

**When NOT to** — if you don't need gRPC semantics, plain JSON-over-HTTP routes are far
simpler.

```cpp
#include <qbuem/server/grpc_handler.hpp>
using namespace qbuem;

GrpcHandler<HelloRequest, HelloReply> handler({
    .service_name = "hello.Greeter",
    .method_name  = "SayHello",
});

handler
  .set_serializer(
      [](const HelloReply& r) { return r.SerializeToBytes(); },          // SerializeFn
      [](std::span<const uint8_t> b) { return HelloRequest::Parse(b); }) // DeserializeFn
  .set_unary([](HelloRequest req) -> Task<Result<HelloReply>> {
      HelloReply rep; rep.message = "Hello, " + req.name;
      co_return rep;                                                      // or std::unexpected(...)
  });

// In your HTTP/2 request handler, after collecting one length-prefixed message:
size_t consumed = 0;
auto msg = GrpcHandler<HelloRequest, HelloReply>::decode_message(frame_bytes, consumed);
if (msg) {
    auto resp = co_await handler.dispatch_unary(*msg);     // Task<Result<GrpcMessage>>
    if (resp) {
        auto wire = GrpcHandler<HelloRequest, HelloReply>::encode_message(*resp);
        // write `wire` as a DATA frame, then a trailer:
        std::string trailer =
            GrpcHandler<HelloRequest, HelloReply>::status_trailer(GrpcStatus::OK);
    }
}
```

Public surface:

| Group | Members |
|---|---|
| Config (ctor arg) | `service_name`, `method_name`, `max_recv_message_size` (4 MiB), `max_send_message_size` (4 MiB) |
| Register codec | `set_serializer(SerializeFn, DeserializeFn)` |
| Register handlers | `set_unary(UnaryFn)`, `set_server_stream(ServerStreamFn)`, `set_client_stream(ClientStreamFn)`, `set_bidi(BidiFn)` |
| Dispatch | `Task<Result<GrpcMessage>> dispatch_unary(const GrpcMessage&)`, `Task<Result<void>> dispatch_server_stream(const GrpcMessage&, Stream<Res>&)`, `Task<Result<GrpcMessage>> dispatch_client_stream(AsyncChannel<Req>&)`, `Task<Result<void>> dispatch_bidi(AsyncChannel<Req>&, Stream<Res>&)` |
| Framing (static) | `encode_message(const GrpcMessage&)`, `decode_message(span, size_t& consumed)` |
| Trailers/path (static/accessor) | `status_trailer(GrpcStatus, string_view = "")`, `grpc_path()` (`/service/method`), `has_unary()/has_server_stream()/has_client_stream()/has_bidi()` |

Function-type aliases: `UnaryFn = Task<Result<Res>>(Req)`,
`ServerStreamFn = Task<void>(Req, Stream<Res>&)`,
`ClientStreamFn = Task<Result<Res>>(AsyncChannel<Req>&)`,
`BidiFn = Task<void>(AsyncChannel<Req>&, Stream<Res>&)`,
`SerializeFn = std::vector<uint8_t>(const Res&)`,
`DeserializeFn = Req(std::span<const uint8_t>)`.

`GrpcMessage { bool compressed; std::vector<uint8_t> payload; }`.
`GrpcStatus`: `OK=0, CANCELLED=1, UNKNOWN=2, INVALID_ARGUMENT=3, NOT_FOUND=5, ALREADY_EXISTS=6,
PERMISSION_DENIED=7, RESOURCE_EXHAUSTED=8, FAILED_PRECONDITION=9, ABORTED=10, INTERNAL=13,
UNAVAILABLE=14`.
`Stream<Res>`: virtual `Task<Result<void>> send(Res)`, `bool is_open() const`.
`AsyncChannel<Req>`: virtual `Task<Result<std::optional<Req>>> recv()` (`nullopt` = client
half-close).

Full four-pattern walkthrough: `examples/02-network/grpc/grpc_example.cpp`.

**Gotchas**
- If `set_serializer()` was not called, every `dispatch_*` returns
  `errc::function_not_supported`. Same if the matching `set_*` handler is unset.
- Messages over `max_recv/send_message_size` return `errc::message_size` (maps to gRPC
  `RESOURCE_EXHAUSTED` — emit that via `status_trailer`).
- `decode_message()` with `< header+payload` bytes returns
  `errc::resource_unavailable_try_again` and `consumed = 0` — buffer more and retry.

---

## 13. Generic connection abstractions (`server/connection_handler.hpp`)

These are the interfaces the protocol handlers above implement. Use them when building a
custom protocol server.

| Type | Role |
|---|---|
| `IConnectionHandler<Frame>` | Per-connection interface: `Task<void> on_connect(int fd, SocketAddr)`, `Task<Result<void>> on_frame(Frame)`, `Task<void> on_disconnect(std::error_code)`. Lifecycle: connect → (repeat) frame → disconnect. Returning an error from `on_frame` closes the connection. |
| `AcceptLoop<Frame, HandlerFactory>` | `SO_REUSEPORT` accept loop scaffold. `Config { SocketAddr addr; HandlerFactory factory; Dispatcher* dispatcher = nullptr; size_t backlog = 1024; }`. `Task<void> run()`, `void stop()`. `factory` returns `std::unique_ptr<IConnectionHandler<Frame>>` per connection. |
| `ConnectionPool<T>` | Outbound pool for connection type `T`. `Config { SocketAddr addr; size_t min_idle=2; size_t max_size=32; uint64_t idle_timeout_ms=30000; std::function<Task<Result<bool>>(T&)> health_check; }`. `Task<Result<Handle>> acquire()`, `void release(T*)`, `idle_count()`, `total_count()`. `Handle` is an RAII move-only wrapper that returns the connection on destruction. |

```cpp
auto factory = [router]{ return std::make_unique<Http1Handler>(router); };
AcceptLoop<http::Request, decltype(factory)> loop({
    .addr       = *SocketAddr::from_ipv4("0.0.0.0", 8080),
    .factory    = factory,
    .dispatcher = &dispatcher,
});
co_await loop.run();
```

> **Note:** `AcceptLoop::run()` is a scaffold — the platform accept/read integration is wired
> by the concrete listener (`App` provides the production accept loop). For a fully-worked
> hand-rolled async TCP server (accept → spawn → echo) see
> `examples/02-network/tcp_echo_server/tcp_echo_server.cpp`.

---

## 14. The curl-free `fetch()` client (HTTP only)

**What it is** — a coroutine-native, **zero-dependency** HTTP/1.1 client built on `TcpStream`
+ async DNS. No libcurl, no OpenSSL. `fetch(url)` returns a chainable `FetchRequest`; `send()`
returns `Task<Result<FetchResponse>>` which you can chain monadically.

**When to use it** — server-to-server calls, health pings, webhooks, talking to a sidecar or
an internal service over plain HTTP.

> **HTTPS is intentionally out of scope.** TLS would require a third-party library, which the
> zero-dependency core does not bundle. Calling an `https://` URL returns
> `errc::protocol_not_supported`. Terminate TLS at a reverse proxy / sidecar, or wrap a TLS
> library at the app layer. Also: HTTP/1.1 only; chunked **request** bodies are not supported.

### 14.1 `fetch()` — one-shot (Connection: close)

```cpp
#include <qbuem/http/fetch.hpp>
using namespace qbuem;

Task<void> example(std::stop_token st) {
    auto resp = co_await fetch("http://api.internal:8080/users/1")
        .header("Accept", "application/json")
        .timeout(std::chrono::seconds{5})
        .send(st);

    if (!resp) {                                   // network/DNS/connect/parse error
        std::println("error: {}", resp.error().message());
        co_return;
    }
    std::println("status={} ok={} len={}",
                 resp->status(), resp->ok(), resp->body().size());
}
```

`FetchRequest` builder methods (all return `FetchRequest&`):

| Method | Effect |
|---|---|
| `method(Method)` / `get()/post()/put()/del()/patch()` | set verb (default GET) |
| `header(key, value)` | add a request header |
| `body(std::string_view)` | set request body (adds `Content-Length`) |
| `timeout(std::chrono::milliseconds)` (or any duration) | total deadline incl. DNS+connect+I/O; ≤0 disables |
| `max_redirects(int n)` | follow up to `n` 3xx hops (default 0 = no follow; 303 → GET) |
| `Task<Result<FetchResponse>> send(const std::stop_token& = {})` | execute |

`FetchResponse`: `int status()`, `bool ok()` (2xx), `std::string_view header(key)`
(case-insensitive), `std::string_view body()`, `std::string take_body() &&` (move out).

### 14.2 Monadic chaining

`Result<FetchResponse>` supports the C++23 `std::expected` monadic ops:

```cpp
auto resp = co_await fetch("http://api.internal/status/200").send(st);

int code = resp.transform([](const FetchResponse& r){ return r.status(); })
               .value_or(-1);

auto body = resp
    .and_then([](const FetchResponse& r) -> Result<std::string> {
        if (!r.ok()) return std::unexpected(std::make_error_code(std::errc::protocol_error));
        return std::string(r.body());
    })
    .transform([](const std::string& b){ return "got: " + b; })
    .value_or("(error)");

// remap errors:
auto norm = resp.transform_error([](std::error_code ec){
    return std::make_error_code(std::errc::host_unreachable);
});
```

### 14.3 `ParsedUrl` — direct URL parsing

```cpp
auto u = ParsedUrl::parse("http://api.example.com:8080/v1/users?x=1");
if (u) { /* u->scheme, u->host, u->port, u->path, u->port_str() */ }
// Defaults: http→80, https→443. IPv6 literals "http://[::1]:9090/" supported.
// Bad scheme/host → errc::invalid_argument.
```

### 14.4 `FetchClient` — pooled connections + Keep-Alive

`fetch()` uses `Connection: close` (new TCP connection per request). `FetchClient` keeps a
per-host pool of idle `TcpStream`s and reuses them, eliminating the handshake/DNS cost on
repeat calls.

```cpp
#include <qbuem/http/fetch_client.hpp>

FetchClient client;
client.set_max_idle_per_host(4);                 // default 4
client.set_timeout(std::chrono::seconds{10});    // default 0 = none
client.set_max_redirects(3);                      // default 5

auto r1 = co_await client.request("http://svc:8080/get").send(st);   // cold: DNS+connect
auto r2 = co_await client.request("http://svc:8080/uuid").get().send(st); // reuses socket
std::println("idle now: {}", client.idle_count());

auto r3 = co_await client.request("http://svc:8080/post")
    .post().header("Content-Type", "application/json").body(R"({"k":"v"})")
    .send(st);
```

`ClientRequest` mirrors `FetchRequest`'s builder (`method/get/post/put/del/patch`, `header`,
`body`, `send`). `FetchClient` also offers `clear_pool()` and `idle_count()`.

**Gotchas**
- `FetchClient` is **not thread-safe** — use one instance per reactor thread (it owns sockets;
  it's movable but not copyable).
- On a stale pooled connection (write fails), the client transparently retries once on a fresh
  connection.
- The whole client honors the caller's `stop_token` *and* an internal timeout timer combined;
  a fired timeout surfaces as `operation_canceled`.
- Response read is capped at 8 MiB (`errc::message_size` beyond that).

Eight worked patterns (GET, POST, monadic, error handling, timeout, redirect, URL parse,
pooling) are in `examples/02-network/http_fetch/http_fetch_example.cpp`.

---

## 15. Retry & backoff helpers (`qbuem::backoff`)

**What it is** — composable backoff policy functions, a `RetryConfig`, and a
**reactor-integrated** `async_sleep()` that suspends the coroutine without blocking the event
loop (it registers a `Reactor` timer; falls back to a blocking sleep only off-reactor, e.g. in
unit tests).

**When to use it** — wrapping `fetch()`/`FetchClient` calls (or any `Task`) with retries on
transient failures (network error, 429, 5xx).

```cpp
#include <qbuem/http/backoff.hpp>
using namespace qbuem;
using namespace qbuem::backoff;
using namespace std::chrono_literals;

Task<Result<FetchResponse>> fetch_with_retry(std::string url, std::stop_token st) {
    RetryConfig cfg{ .max_attempts = 4, .policy = jitter(200ms, 30s) };
    Result<FetchResponse> last = std::unexpected(std::make_error_code(std::errc::io_error));

    for (int attempt = 0; attempt < cfg.max_attempts; ++attempt) {
        last = co_await fetch(url).send(st);
        int status = last ? last->status() : 0;        // 0 = network error
        if (last && !cfg.retryable(status)) co_return last;   // success / non-retryable
        if (attempt + 1 < cfg.max_attempts)
            co_await async_sleep(cfg.policy(attempt));        // non-blocking backoff
    }
    co_return last;
}
```

Backoff factories (all return `BackoffFn = std::function<std::chrono::milliseconds(int attempt)>`):

| Factory | Delay shape |
|---|---|
| `fixed(delay)` | constant |
| `linear(base, cap = 30s)` | `base * (attempt+1)`, capped |
| `exponential(base, cap = 30s)` | `base * 2^attempt`, capped |
| `jitter(base = 500ms, cap = 30s)` | AWS full jitter: `random(0, min(cap, base*2^attempt))` — recommended for distributed retries |
| `decorrelated(base = 500ms, cap = 30s)` | `random(base, prev*3)`, capped |

`RetryConfig { int max_attempts = 3; BackoffFn policy = jitter(); std::function<bool(int)> retryable; }`
— default `retryable` retries on status `0` (network error), `429`, and any `>= 500`.

`Task<void> async_sleep(std::chrono::milliseconds)` — yields to the reactor for the delay.

**Gotchas**
- `async_sleep()` only suspends cooperatively when a `Reactor` is current; off-reactor it
  blocks the thread (acceptable in tests, never on a hot path).
- `jitter`/`decorrelated` use a `thread_local` RNG; `decorrelated` keeps mutable state in a
  shared `prev`, so a given `BackoffFn` instance is stateful across calls.

---

## 16. `HttpParser` — the HTTP/1.1 request parser

**What it is** — the state-machine parser `App` uses to turn raw socket bytes into a
`Request`. You only need it directly when writing your own transport.

```cpp
#include <qbuem/http/parser.hpp>
using namespace qbuem;

HttpParser parser;                       // one parser per request (re-create per request)
Request req;
std::optional<size_t> n = parser.parse(received_bytes, req);   // bytes consumed, or nullopt on error
if (!n) {
    int code = parser.error_status();    // 400 (bad syntax) or 413 (payload too large)
}
if (parser.is_complete()) { /* dispatch req */ }
if (parser.headers_complete()) { /* e.g. send 100 Continue before the body */ }
```

| Member | Returns | Notes |
|---|---|---|
| `std::optional<size_t> parse(std::string_view, Request&)` | bytes consumed | `nullopt` = hard error |
| `bool is_complete() const` / `State state() const` | — | `State` enum incl. `Method, Path, …, Body, ChunkSize, …, Complete, Error` |
| `bool headers_complete() const` | — | true once headers parsed (body may be pending) |
| `int error_status() const` | 400 / 413 | what to send when `parse()` returns `nullopt` |

Supports fixed-length (`Content-Length`) and chunked bodies. Limits:
`MAX_BODY_SIZE = 1 MiB` (→ 413), `MAX_HEADER_SIZE = 8 KiB`.

**Gotcha** — a parser is stateful and persistent across `parse()` calls on the *same*
accumulating buffer for one request; create a fresh `HttpParser` per request. For ordinary
servers, `App` does all of this for you.

---

## Choosing the right tool (quick decision table)

| Goal | Use |
|---|---|
| Serve HTTP/1.1 with routing, middleware, static files, health/metrics | `App` (§1) |
| Run several listeners in one process | `StackController` (§2) |
| Server-rendered HTML fragments | `qbuem::http::TemplateEngine` (§8) |
| Distributed tracing propagation | `qbuem::http::make_trace_middleware` (§7) |
| Call another HTTP service once | `fetch()` (§14.1) |
| Many calls to the same host | `FetchClient` (§14.4) |
| Retry transient failures | `qbuem::backoff` + a loop (§15) |
| Real-time bidirectional channel | `WebSocketHandler` via `Http1Handler` upgrade (§9, §10) |
| HTTP/2 framing / gRPC plumbing | `Http2Handler` (§11) + `GrpcHandler` (§12) |
| Build a custom protocol server | `IConnectionHandler` / `AcceptLoop` (§13) or raw `TcpListener`/`TcpStream` (`tcp_echo_server`) |

All of the above are POSIX-only and run identically on Linux x86_64, ARM64/Jetson, and macOS
aarch64; SIMD paths (WebSocket masking) auto-select AVX2/SSE2 on x86_64 and NEON on AArch64,
with a scalar fallback elsewhere.
