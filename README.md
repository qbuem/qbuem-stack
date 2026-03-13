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
└── Layer 4: App Lifecycle   — App, StackController, 헬스체크, 메트릭
```

**외부 의존성 주입 포인트 (추상 인터페이스):**

| 기능 | 인터페이스 | 서비스에서 구현 |
|------|-----------|---------------|
| 압축 (gzip/br/zstd) | `IBodyEncoder` | zlib, brotli, zstd 등 선택 |
| 인증 (JWT/API Key) | `ITokenVerifier` | OpenSSL, mbedTLS, 자체 구현 등 |
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
#include <draco/draco.hpp>
#include <draco/middleware/cors.hpp>
#include <draco/middleware/rate_limit.hpp>
#include <draco/middleware/security.hpp>
#include <draco/middleware/request_id.hpp>

int main() {
    draco::App app;

    // ── 미들웨어 ─────────────────────────────────────────────────────────
    app.use(draco::middleware::request_id());
    app.use(draco::middleware::secure_headers());
    app.use(draco::middleware::cors());
    app.use(draco::middleware::rate_limit({.rate_per_sec = 1000, .burst = 50}));

    // ── 동기 핸들러 ──────────────────────────────────────────────────────
    app.get("/hello", [](const draco::Request& req, draco::Response& res) {
        res.status(200).body("hello");
    });

    // ── 비동기 코루틴 핸들러 ─────────────────────────────────────────────
    app.get("/user/:id", [](const draco::Request& req, draco::Response& res)
        -> draco::Task<void> {
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
    app.on_error([](std::exception_ptr ep, const draco::Request&, draco::Response& res) {
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
#include <draco/middleware/body_encoder.hpp>

class GzipEncoder : public draco::middleware::IBodyEncoder {
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
app.get("/data", [&gzip](const draco::Request& req, draco::Response& res) {
    res.status(200).header("Content-Type", "application/json").body(big_json);
    draco::middleware::compress_response(gzip, req, res);
});
```

### 인증 (`ITokenVerifier`)

```cpp
#include <draco/middleware/token_auth.hpp>

class MyJwtVerifier : public draco::middleware::ITokenVerifier {
public:
    std::optional<draco::middleware::TokenClaims>
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
app.use(draco::middleware::bearer_auth(verifier));

app.get("/me", [](const draco::Request&, draco::Response& res) {
    auto sub = res.get_header("X-Auth-Sub"); // 검증된 subject 클레임
    res.status(200).body(sub);
});
```

### SSE (Server-Sent Events)

```cpp
#include <draco/middleware/sse.hpp>

app.get("/events", draco::Handler([](const draco::Request&, draco::Response& res) {
    draco::SseStream sse(res);
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
draco::App api, admin;
// ... 라우트 등록 ...

draco::StackController ctrl;
ctrl.add(api,   8080);
ctrl.add(admin, 9090);
ctrl.run();   // SIGTERM/SIGINT 자동 처리
```

---

## JSON 통합 (qbuem-json)

`qbuem-stack` 코어는 JSON에 **의존하지 않습니다**.  
권장 라이브러리: **[qbuem-json](https://github.com/qbuem/qbuem-json)**

```cpp
#include <qbuem_json/qbuem_json.hpp>
#include <draco/draco.hpp>

app.post("/echo", draco::Handler([](const draco::Request& req, draco::Response& res) {
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

---

## 테스트

```bash
# 빌드 + 전체 테스트
cmake -B build && cmake --build build --parallel
ctest --test-dir build -V

# 개별 실행
./build/tests/draco_tests       # 코어 + HTTP (60 tests)
./build/tests/qbuem_json_tests  # qbuem-json 통합 (네트워크 필요)
```

---

## 알려진 제한

- Windows 미지원 (POSIX syscall 전용)
- io_uring SQPOLL은 `CAP_SYS_NICE` 또는 root 권한 필요
- macOS: `kern.maxfilesperproc` 조정으로 최대 fd 수 확장 필요

---

## 로드맵

전체 계획은 **[TODO.md](./TODO.md)** 참조.

---

*qbuem-stack — by The LKB Innovations*
