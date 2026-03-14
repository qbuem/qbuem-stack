# qbuem-stack

**Zero Latency · Zero Cost · Zero Dependency**

C++20 WAS (Web Application Server) 레이어 라이브러리.

> `qbuem-stack`은 실제 서비스의 기반이 되는 **프레임워크 레이어**입니다.  
> 외부 라이브러리 의존성이 **전혀 없습니다** — OS 커널 API만 사용합니다.  
> 압축, 인증, DB 연동 등 서비스 고유 로직은 **추상 인터페이스**로 주입합니다.

---

## 핵심 원칙

| 원칙 | 구현 방법 |
|------|-----------|
| **Zero Latency** | SIMD HTTP 파서 · sendfile(2) zero-copy · TCP_CORK · TCP_FASTOPEN · SO_BUSY_POLL |
| **Zero Cost** | C++20 코루틴 · CRTP · LTO · `[[likely]]`/`[[unlikely]]` · vDSO 타이밍 |
| **Zero Dependency** | 커널 syscall만 사용 · 외부 라이브러리 링크 없음 · 추상 인터페이스로 확장 |
| **Low Memory** | Per-request Arena · FixedPoolResource · SPSC ring-buffer 로거 · cache-line 정렬 |

---

## 레이어 구조

```
qbuem-stack
├── Layer 0: Common          — Result<T>, Status, PMR Arena, 상수
├── Layer 1: IO Core         — IReactor (io_uring / epoll / kqueue), Dispatcher, Task<T>
├── Layer 2: HTTP            — SIMD 파서, Radix Tree 라우터, Request/Response
├── Layer 3: Middleware      — CORS, RateLimit, Security, RequestID, SSE, Static
├── Layer 4: App Lifecycle   — App, StackController, 헬스체크, 메트릭
└── Layer 5: Pipeline        — AsyncChannel, Action, StaticPipeline / DynamicPipeline,
                               PipelineGraph, MessageBus, Tracing, Resilience
```

**외부 의존성 주입 포인트 (추상 인터페이스):**

| 기능 | 인터페이스 | 서비스에서 구현 |
|------|-----------|---------------|
| 압축 (gzip/br/zstd) | `IBodyEncoder` | zlib, brotli, zstd 등 선택 |
| 인증 (JWT/API Key) | `ITokenVerifier` | OpenSSL, mbedTLS, 자체 구현 등 |
| 분산 트레이싱 | `SpanExporter` | OTLP, Jaeger, Zipkin, 자체 구현 |
| DB 연결 | 서비스 자체 설계 | libpq, mariadb, redis 등 |
| TLS | 서비스 자체 설계 | OpenSSL, mbedTLS 등 |

---

## 빌드

### 요구사항

| 항목 | 최소 버전 |
|------|---------|
| 컴파일러 | GCC 13+ / Clang 16+ |
| CMake | 3.20+ |
| OS | Linux / macOS |
| 커널 (Linux) | 5.11+ (io_uring POLL_ADD) |

### 빌드 명령

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build          # 단위 테스트 (qbuem-json 포함)
```

### CMake 타겟

```cmake
find_package(qbuem-stack REQUIRED)

target_link_libraries(my_service
    qbuem-stack::qbuem   # 전체 (HTTP + IO + Lifecycle)
    # 또는 선택적으로:
    # qbuem-stack::core  — IO 레이어만
    # qbuem-stack::http  — HTTP 파서 + 라우터만
)
```

---

## 빠른 시작

```cpp
#include <qbuem/qbuem-stack.hpp>
#include <qbuem/middleware/cors.hpp>
#include <qbuem/middleware/rate_limit.hpp>
#include <qbuem/middleware/security.hpp>
#include <qbuem/middleware/request_id.hpp>

int main() {
    qbuem::App app;

    // ── 미들웨어 ─────────────────────────────────────────────────────────
    app.use(qbuem::middleware::request_id());
    app.use(qbuem::middleware::secure_headers());
    app.use(qbuem::middleware::cors());
    app.use(qbuem::middleware::rate_limit({.rate_per_sec = 1000, .burst = 50}));

    // ── 동기 핸들러 ──────────────────────────────────────────────────────
    app.get("/hello", [](const qbuem::Request& req, qbuem::Response& res) {
        res.status(200).body("hello");
    });

    // ── 비동기 코루틴 핸들러 ─────────────────────────────────────────────
    app.get("/user/:id", [](const qbuem::Request& req, qbuem::Response& res)
        -> qbuem::Task<void> {
        auto id = req.param("id");
        res.status(200)
           .header("Content-Type", "application/json")
           .body("{\"id\":\"" + std::string(id) + "\"}");
        co_return;
    });

    // ── 내장 엔드포인트 ──────────────────────────────────────────────────
    app.health_check("/health");
    app.health_check_detailed("/health/detail");
    app.liveness_endpoint("/live");
    app.readiness_endpoint("/ready");
    app.metrics_endpoint("/metrics");
    app.serve_static("/static", "./www");

    // ── 글로벌 에러 핸들러 ───────────────────────────────────────────────
    app.on_error([](std::exception_ptr ep, const qbuem::Request&, qbuem::Response& res) {
        try { std::rethrow_exception(ep); }
        catch (const std::exception& e) {
            res.status(500)
               .header("Content-Type", "application/json")
               .body(std::string("{\"error\":\"") + e.what() + "\"}");
        }
    });

    // ── 연결 수 제한, 로그 ───────────────────────────────────────────────
    app.set_max_connections(50000);
    app.enable_access_log();   // 텍스트 로그
    // app.enable_json_log();  // JSON 구조화 로그

    return app.listen(8080) ? 0 : 1;
}
```

---

## 외부 기능 주입

### 압축 (`IBodyEncoder`)

```cpp
#include <zlib.h>
#include <qbuem/middleware/body_encoder.hpp>

class GzipEncoder : public qbuem::middleware::IBodyEncoder {
public:
    bool encode(std::string_view src, std::string &dst) noexcept override {
        // zlib deflateInit2(...) + deflate + deflateEnd
        // ...
        return true;
    }
    std::string_view encoding_name() const noexcept override { return "gzip"; }
    std::string_view accept_token() const noexcept override  { return "gzip"; }
};

// 사용:
GzipEncoder gzip;
app.get("/data", [&gzip](const qbuem::Request& req, qbuem::Response& res) {
    res.status(200).header("Content-Type", "application/json").body(big_json);
    qbuem::middleware::compress_response(gzip, req, res);
});
```

### 인증 (`ITokenVerifier`)

```cpp
#include <qbuem/middleware/token_auth.hpp>

class MyJwtVerifier : public qbuem::middleware::ITokenVerifier {
public:
    std::optional<qbuem::middleware::TokenClaims>
    verify(std::string_view token) noexcept override {
        // 1. base64url 분리 → header.payload.sig
        // 2. HMAC-SHA256 서명 검증 (constant_time_equal)
        // 3. exp / nbf / iss 클레임 검증
        // 4. TokenClaims 반환
        return std::nullopt; // 실패 시
    }
};

// 사용:
MyJwtVerifier verifier;
app.use(qbuem::middleware::bearer_auth(verifier));

app.get("/me", [](const qbuem::Request&, qbuem::Response& res) {
    auto sub = res.get_header("X-Auth-Sub"); // 검증된 subject 클레임
    res.status(200).body(sub);
});
```

### SSE (Server-Sent Events)

```cpp
#include <qbuem/middleware/sse.hpp>

app.get("/events", qbuem::Handler([](const qbuem::Request&, qbuem::Response& res) {
    qbuem::SseStream sse(res);
    sse.send("connected", "status");
    sse.send("42",        "counter", "1");
    sse.heartbeat();
    sse.close();
}));
```

### Unix Domain Socket

```cpp
// TCP 오버헤드 없이 로컬 IPC (nginx 리버스 프록시, 컨테이너 간)
auto result = app.listen_unix("/run/myapp.sock");
```

### StackController (다중 App)

```cpp
qbuem::App api, admin;
// ... 라우트 등록 ...

qbuem::StackController ctrl;
ctrl.add(api,   8080);
ctrl.add(admin, 9090);
ctrl.run();   // SIGTERM/SIGINT 자동 처리
```

---

## Pipeline (예정 — Layer 5)

> 설계 문서: **[docs/pipeline-design.md](./docs/pipeline-design.md)**

비동기 처리 파이프라인. 코루틴 기반 워커 풀 + lock-free 채널로 구성.
Action 단위 독립 scale-in/out, backpressure 자동 전파, fan-out/in 배선.

### 정적 파이프라인 (StaticPipeline) — 컴파일타임 타입 체인

```cpp
#include <qbuem/pipeline/pipeline.hpp>

// add() 호출마다 반환 타입이 바뀜 — 타입 불일치 시 컴파일 에러
auto pipeline =
    qbuem::PipelineBuilder<HttpRequest>{}
        .add(auth_action)       // Action<HttpRequest, AuthedReq>
        .add(parse_action)      // Action<AuthedReq, ParsedBody>
        .add(persist_action)    // Action<ParsedBody, DbResult>
        .build();               // StaticPipeline<HttpRequest, DbResult>

pipeline.start(dispatcher);
co_await pipeline.push(request);         // backpressure: 포화 시 co_await 대기
co_await pipeline.drain();               // graceful shutdown
```

### 동적 파이프라인 (DynamicPipeline) — 런타임 구성

```cpp
qbuem::DynamicPipeline dp;
dp.add_action("parse",   make_dynamic_action(parse_fn));
dp.add_action("enrich",  make_dynamic_action(enrich_fn));
dp.add_action("persist", make_dynamic_action(persist_fn));
dp.start(dispatcher);

// Hot-swap — 재시작 없이 로직 교체 (in-flight 아이템 유실 없음)
co_await dp.hot_swap("enrich", make_dynamic_action(new_enrich_fn));
```

### 파이프라인 그래프 (PipelineGraph) — DAG 오케스트레이션

```cpp
qbuem::PipelineGraph graph;
graph.add("ingest",  ingestion_pipeline);
graph.add("process", processing_pipeline);
graph.add("notify",  notification_pipeline);
graph.add("audit",   audit_pipeline);

graph.connect("ingest", "process");
graph.fan_out("process", {"notify", "audit"});  // 완료 후 복수 트리거

graph.start(dispatcher);   // Kahn's algorithm으로 DAG 검증 후 위상 정렬 시작
```

### 메시지 패턴 (MessageBus) — gRPC 스타일

```cpp
qbuem::MessageBus bus;
bus.create_unary<OrderReq, OrderRes>("order-service", /*cap=*/64);

// 요청 측
auto reply_ch = std::make_shared<qbuem::AsyncChannel<qbuem::Result<OrderRes>>>(1);
co_await bus.channel<OrderReq, OrderRes>("order-service").send({req, reply_ch});
auto result = co_await reply_ch->recv();

// 서비스 측 (파이프라인 첫 Action에서)
auto env = co_await bus.channel<OrderReq, OrderRes>("order-service").recv();
co_await env.reply->send(co_await process_order(env.request));
```

### 분산 트레이싱

```cpp
// W3C Trace Context — HTTP traceparent 헤더와 자동 연결
auto tracer = std::make_shared<qbuem::PipelineTracer>(
    std::make_shared<qbuem::ProbabilitySampler>(0.01),   // 1% 샘플링
    std::make_shared<qbuem::OtlpGrpcExporter>("localhost:4317")
);
qbuem::set_global_tracer(tracer);
// 이후 모든 파이프라인 처리 시 자동으로 span 생성/전파
```

---

## JSON 통합 (qbuem-json)

`qbuem-stack` 코어는 JSON에 **의존하지 않습니다**.  
권장 라이브러리: **[qbuem-json](https://github.com/qbuem/qbuem-json)**

```cpp
#include <qbuem_json/qbuem_json.hpp>
#include <qbuem/qbuem-stack.hpp>

app.post("/echo", qbuem::Handler([](const qbuem::Request& req, qbuem::Response& res) {
    qbuem::Document doc;
    auto body = qbuem::parse(doc, req.body());

    // SafeValue 체인 — 키 없거나 타입 불일치 시 fallback 자동 전파
    std::string msg  = body.get("message") | std::string("(no message)");
    std::string name = body.get("name")    | std::string("anonymous");

    qbuem::Document out_doc;
    auto out = qbuem::parse(out_doc, "{}");
    out.insert("echo",   msg);
    out.insert("from",   name);
    out.insert("ok",     true);

    res.status(200)
       .header("Content-Type", "application/json")
       .body(out.dump());
}));
```

CMake 통합:

```cmake
include(FetchContent)
FetchContent_Declare(qbuem_json
    GIT_REPOSITORY https://github.com/qbuem/qbuem-json.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(qbuem_json)
target_link_libraries(my_service PRIVATE qbuem-stack::qbuem qbuem_json::qbuem_json)
```

---

## 구현 현황

| 구성요소 | 상태 | 비고 |
|---------|------|------|
| kqueue Reactor (macOS) | ✅ | `EVFILT_READ/WRITE/TIMER` |
| epoll Reactor (Linux) | ✅ | `epoll` + `timerfd` |
| io_uring Reactor (Linux) | ✅ | POLL_ADD 기반 |
| Thread-per-core Dispatcher | ✅ | CPU affinity |
| SIMD HTTP/1.1 파서 | ✅ | AVX2 / SSE2 / NEON / scalar |
| Radix Tree 라우터 | ✅ | `/:param`, prefix 라우트 |
| Keep-Alive / Pipelining | ✅ | 최대 100 req/conn |
| Chunked Transfer Encoding | ✅ | `Response::chunk()` |
| Server-Sent Events | ✅ | `SseStream` |
| writev() zero-copy send | ✅ | header+body 단일 syscall |
| sendfile(2) | ✅ | Linux + macOS zero-copy |
| TCP_CORK / TCP_QUICKACK | ✅ | segment 병합 |
| TCP_FASTOPEN | ✅ | Linux listen socket |
| SO_BUSY_POLL (50µs) | ✅ | Linux 수신 지연 최소화 |
| SO_SNDTIMEO (5s) | ✅ | 느린 클라이언트 차단 방지 |
| Unix Domain Socket | ✅ | `listen_unix()` |
| 정적 파일 서빙 | ✅ | ETag, Last-Modified, Range |
| CORS 미들웨어 | ✅ | 단일 origin + whitelist |
| Rate Limit | ✅ | 토큰 버킷, thread-local |
| Security Headers | ✅ | HSTS, CSP, X-Frame-Options |
| Request ID | ✅ | UUID v4 |
| 압축 | ✅ | `IBodyEncoder` 추상 인터페이스 |
| 인증 | ✅ | `ITokenVerifier` 추상 인터페이스 |
| SPSC Async Logger | ✅ | Text / JSON 포맷 |
| FixedPoolResource | ✅ | O(1) slab allocator |
| StackController | ✅ | 다중 App 라이프사이클 |
| K8s Probe | ✅ | `/live`, `/ready` |
| 연결 수 제한 | ✅ | 초과 시 503 + Retry-After |
| 에러 핸들러 | ✅ | `on_error(ErrorHandler)` |
| TLS/SSL | ❌ 예정 | `ITransport` 추상화로 주입 |
| HTTP/2 | ❌ 예정 | |
| WebSocket | ❌ 예정 | |
| **Pipeline** | | |
| `Reactor::post()` / `Dispatcher::spawn()` | ❌ 예정 | cross-thread 작업 주입 |
| `AsyncChannel<T>` (MPMC ring) | ❌ 예정 | Dmitry Vyukov 알고리즘 |
| `PriorityChannel<T>` | ❌ 예정 | 3레벨 + aging |
| `Stream<T>` | ❌ 예정 | `variant<T, StreamEnd>` EOS |
| `Action<In,Out>` (정적) | ❌ 예정 | 코루틴 워커 풀, scale-in/out |
| `IDynamicAction` (동적) | ❌ 예정 | 타입 소거 + ActionSchema |
| `StaticPipeline<In,Out>` | ❌ 예정 | 컴파일타임 타입 체인 |
| `DynamicPipeline` | ❌ 예정 | 런타임 구성, hot-swap |
| `PipelineGraph` | ❌ 예정 | DAG 오케스트레이션 |
| `MessageBus` | ❌ 예정 | Unary / Stream / Bidi |
| `RetryPolicy` / `CircuitBreaker` | ❌ 예정 | 복원력 패턴 |
| `DeadLetterQueue` | ❌ 예정 | 실패 아이템 격리 |
| `PipelineTracer` (W3C Trace Context) | ❌ 예정 | OpenTelemetry 호환 |
| `PipelineFactory` (Config-driven) | ❌ 예정 | JSON/YAML → Pipeline |
| **Pipeline 고급** | | |
| `TaskGroup` (구조적 동시성) | ❌ 예정 | 자식 코루틴 수명 관리, cancel-on-error |
| `SpscChannel<T>` | ❌ 예정 | 1:1 고속 채널 (MPMC 대비 50% atomic 절감) |
| Batch ops (`send_batch`/`recv_batch`) | ❌ 예정 | DB bulk insert 등 처리량 최적화 |
| Rx-style 스트림 연산자 | ❌ 예정 | map/filter/flat_map/zip/merge/chunk |
| `WindowedAction<T>` | ❌ 예정 | Tumbling/Sliding/Session 창 집계 |
| `ScatterGatherAction` | ❌ 예정 | 병렬 서브작업 후 결과 집계 |
| `DebounceAction` / `ThrottleAction` | ❌ 예정 | 이벤트 속도 제어 |
| `SagaOrchestrator` | ❌ 예정 | 실패 시 역순 보상 트랜잭션 |
| `IdempotencyFilter` | ❌ 예정 | Exactly-once 중복 처리 방지 |
| `ICheckpointStore` | ❌ 예정 | ETL/배치 크래시 복구 |
| SLO Tracking + Error Budget | ❌ 예정 | P99 목표 + 자동 circuit break |
| Pipeline Topology Export | ❌ 예정 | JSON/DOT/Mermaid 위상 시각화 |
| Canary 자동화 | ❌ 예정 | gradual rollout + 자동 롤백 |
| Pipeline Versioning | ❌ 예정 | Schema Evolution + MigrationFn |

---

## 테스트

```bash
# 빌드 + 전체 테스트
cmake -B build && cmake --build build --parallel
ctest --test-dir build -V

# 개별 실행
./build/tests/qbuem_tests       # 코어 + HTTP (60 tests)
./build/tests/qbuem_json_tests  # qbuem-json 통합 (네트워크 필요)
```

---

## 문서

모든 공개 API는 **Doxygen** 주석으로 문서화되어 있습니다.

```bash
# 의존성 설치 (Ubuntu/Debian)
sudo apt-get install doxygen graphviz

# HTML 문서 생성
doxygen Doxyfile

# 브라우저로 열기
xdg-open docs/api/html/index.html
```

생성된 문서는 `docs/api/html/` 에 저장됩니다.
각 헤더 파일의 **Doxygen 블록** (`@file`, `@brief`, `@defgroup`) 을 통해
IDE 자동완성(IntelliSense / clangd)도 지원됩니다.

---

## 알려진 제한

- Windows 미지원 (POSIX syscall 전용)
- io_uring SQPOLL은 `CAP_SYS_NICE` 또는 root 권한 필요
- macOS: `kern.maxfilesperproc` 조정으로 최대 fd 수 확장 필요

---

## 로드맵

| 버전 | 주요 내용 |
|------|----------|
| v0.5.0 | `Reactor::post()` + `Dispatcher::spawn()` — 파이프라인 전제조건 |
| v0.6.0 | Pipeline MVP — `Context`, `ServiceRegistry`, `AsyncChannel`, `Action`, `StaticPipeline` |
| v0.7.0 | `DynamicPipeline` + `PipelineGraph` + `MessageBus` |
| v0.8.0 | 복원력 (`RetryPolicy`, `CircuitBreaker`, DLQ) + 트레이싱 |
| v0.9.0 | Hot-swap, Priority Channel, Config-driven, Pipeline 합성 |
| v0.9.1 | Windowing, Saga, Exactly-once, Checkpoint, SLO, Topology, Canary |
| v0.9.2 | NUMA pinning, SPSC, Batch ops, Rx 연산자, Pipeline Versioning |
| v1.0.0 | HTTP/2, WebSocket, gRPC |

전체 계획은 **[TODO.md](./TODO.md)**, 파이프라인 상세 설계는 **[docs/pipeline-design.md](./docs/pipeline-design.md)** 참조.

---

*qbuem-stack — by The LKB Innovations*
