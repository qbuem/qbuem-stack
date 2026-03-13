# ⚡️ qbuem-stack (Zenith Stack v2.5.0)

**C++20 Ultra-Low Latency Web Application Server**

> **Zero Latency · Zero Cost · Low Memory · Low CPU**

`qbuem-stack`은 HFT, 실시간 분석, 초고트래픽 API 환경을 위해 설계된 C++20 Web Application Server 라이브러리입니다. Zenith Stack v2.5.0 사양을 준수하며, 개발 편의성과 시스템 레벨 C++의 극한 성능을 동시에 제공합니다.

---

## 4대 핵심 원칙

| 원칙 | 구현 방법 |
|------|----------|
| **Zero Latency** | io_uring SQPOLL(Linux) · kqueue(macOS) · SIMD HTTP 파서 · MSG_ZEROCOPY · TCP_FASTOPEN |
| **Zero Cost** | C++20 컴파일 타임 추상화 · CRTP 정적 다형성 · LTO/PGO · `[[always_inline]]` |
| **Low Memory** | Shared-Nothing Reactor · Per-request Arena 할당자 · Slab 오브젝트 풀 · 힙 할당 제거 |
| **Low CPU** | Thread-per-core (lock-free) · Cache-line 정렬 · Branch 예측 힌트 · vDSO 타이밍 |

---

## 아키텍처 (Zenith 5-Layer Model)

```
┌─────────────────────────────────────────────────────────┐
│                 Layer 4: Zenith-Controller               │
│            StackController · Lifecycle · Config         │
└────────────────────────┬────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────┐
│                 Layer 3: Zenith-Pipeline                 │
│          Task<T> · IAction · Work-Stealing Scheduler    │
└────────────────────────┬────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────┐
│                   Layer 2: Zenith-WAS                    │
│  Parser (SIMD) → Router (Radix Tree) → Middleware chain  │
└────────────────────────┬────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────┐
│                    Layer 1: Zenith-IO                    │
│          IReactor (io_uring / epoll / kqueue)           │
└────────────────────────┬────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────┐
│                  Layer 0: Zenith-Common                  │
│     Status/Result · PMR Memory · SPSC Queue · Logger    │
└─────────────────────────────────────────────────────────┘
```

**이벤트 루프 플로우 (Linux io_uring)**
```
accept() ──► Connection (Arena 할당)
    │
    ▼
AsyncRead ──► HTTP Parser (SIMD) ──► Router match
    │                                      │
    ▼                                      ▼
Middleware chain ──────────────► co_await Handler
    │
    ▼
AsyncWrite ──► (Keep-Alive? → Idle) / (Close → Arena free)
```

---

## 현재 구현 상태 (v0.3.0)

| 구성요소 | 상태 | 비고 |
|---------|------|------|
| kqueue Reactor (macOS) | ✅ 완료 | `EVFILT_READ/WRITE/TIMER` |
| epoll Reactor (Linux) | ✅ 완료 | `epoll` + `timerfd` |
| io_uring Reactor (Linux) | 🔄 진행 중 | POLL_ADD 기반 구현; SQPOLL은 Phase 5 |
| Thread-per-core Dispatcher | ✅ 완료 | CPU affinity 포함 |
| Radix Tree Router | ✅ 완료 | `/:param` 추출 + Prefix 라우트 |
| HTTP/1.1 Parser | ✅ 완료 | Keep-Alive, Chunked, 100-Continue, Pipelining |
| `zenith::Task<T>` Coroutine | ✅ 완료 | symmetric transfer |
| AsyncRead / AsyncWrite | ✅ 완료 | |
| AsyncSleep (timer) | ✅ 완료 | |
| Middleware Pipeline | ✅ 완료 | Request ID, Rate Limit, CORS, Security Headers, Cookie |
| Static File Serving | ✅ 완료 | ETag, Last-Modified, Range, MIME 자동 감지 |
| Health Check / Metrics | ✅ 완료 | Prometheus `/metrics` 포맷 지원 |
| Conditional Requests | ✅ 완료 | ETag / If-None-Match → 304 Not Modified |
| Range Requests | ✅ 완료 | 206 Partial Content / 416 Range Not Satisfiable |
| Slowloris 완화 | ✅ 완료 | Read timeout 10s (per-request) |
| Write Timeout | ✅ 완료 | `SO_SNDTIMEO` 5s — 느린 클라이언트가 reactor 점유 방지 |
| IPv6 dual-stack | ✅ 완료 | IPv4/IPv6 Dual-stack listen |
| Unix Domain Socket | ✅ 완료 | `App::listen_unix()` — AF_UNIX 로컬 IPC |
| TCP_FASTOPEN | ✅ 완료 | Linux listen socket |
| SO_BUSY_POLL | ✅ 완료 | Linux 50µs busy-poll — 수신 지연 최소화 |
| SO_NOSIGPIPE | ✅ 완료 | macOS client socket |
| sendfile(2) | ✅ 완료 | Linux + macOS 제로카피 정적 파일 전송 |
| Remote Addr | ✅ 완료 | `req.remote_addr()` 지원 |
| Crypto Utils | ✅ 완료 | constant-time compare, CSPRNG, CSRF token |
| Access Log | ✅ 완료 | Combined Log Format 지원 (SPSC ring-buffer 비동기) |
| Async Logger | ✅ 완료 | Lock-free SPSC 큐 기반 비동기 로거 |
| Chunked Transfer | ✅ 완료 | `Response::chunk()` / `end_chunks()` |
| Server-Sent Events | ✅ 완료 | `SseStream` — text/event-stream 스트리밍 |
| gzip 압축 | ✅ 완료 | `compress()` 미들웨어 (zlib) |
| JWT 인증 | ✅ 완료 | HS256 검증, exp/nbf/iss/aud 클레임, timing-safe |
| StackController | ✅ 완료 | 다중 App 라이프사이클 통합 관리 |
| JSON (코어 의존 없음) | ✅ 완료 | 프레임워크 코어에서 완전 제거; 앱이 직접 처리 |
| TLS/SSL | ❌ 예정 | Phase 8 |
| HTTP/2 | ❌ 예정 | Phase 9 |
| HTTP/3 / QUIC | ❌ 예정 | Phase 9 |
| WebSocket | ❌ 예정 | Phase 9 |
| gRPC | ❌ 예정 | Phase 9 |

---

## 빠른 시작

```cpp
#include <draco/draco.hpp>
#include <draco/middleware/cors.hpp>
#include <draco/middleware/request_id.hpp>
#include <draco/middleware/rate_limit.hpp>
#include <draco/middleware/security.hpp>

int main() {
    draco::App app;

    // ── 미들웨어 등록 ──────────────────────────────────────────────
    app.use(draco::middleware::request_id());          // X-Request-ID 자동 생성
    app.use(draco::middleware::secure_headers());      // HSTS, CSP, X-Frame-Options …
    app.use(draco::middleware::cors());                // CORS preflight 자동 처리
    app.use(draco::middleware::rate_limit());          // 토큰 버킷 (100 req/s, burst 20)

    // ── 동기 핸들러 ────────────────────────────────────────────────
    app.get("/hello", [](const draco::Request& req, draco::Response& res) {
        res.status(200)
           .header("Content-Type", "text/plain")
           .body("Hello, qbuem-stack!");
    });

    // ── 비동기 코루틴 핸들러 ───────────────────────────────────────
    app.get("/user/:id", [](const draco::Request& req, draco::Response& res)
        -> draco::Task<void> {
        auto id = req.param("id");
        res.status(200)
           .header("Content-Type", "application/json")
           .body("{\"user_id\":\"" + std::string(id) + "\"}");
        co_return;
    });

    // ── 내장 엔드포인트 ────────────────────────────────────────────
    app.health_check("/health");           // GET /health → {"status":"ok"}
    app.metrics_endpoint("/metrics");      // GET /metrics → Prometheus 포맷
    app.serve_static("/static", "./www");  // 정적 파일 서빙 (ETag + Range 지원)

    // ── 액세스 로그 활성화 ─────────────────────────────────────────
    app.enable_access_log();   // stderr: [2024-…] GET /hello 200 123µs

    // ── 서버 시작 (IPv4 / IPv6 dual-stack) ────────────────────────
    auto result = app.listen(8080);   // IPv4
    // auto result = app.listen(8080, true); // IPv6 dual-stack
    if (!result) {
        std::cerr << "listen failed: " << result.error().message() << "\n";
        return 1;
    }
}
```

### JSON 통합 (qbuem-json)

qbuem-stack 코어는 JSON에 의존하지 않습니다. 앱이 직접 원하는 JSON 라이브러리를 선택합니다.
권장 라이브러리는 **[qbuem-json](https://github.com/qbuem/qbuem-json)** (구 beast-json, 동일 팀 개발)입니다.

```cpp
#include <qbuem_json/qbuem_json.hpp>
#include <draco/draco.hpp>

// POST /echo — qbuem-json SafeValue 체인 시연
draco::Task<void> echo_handler(const draco::Request& req, draco::Response& res) {
    qbuem::Document doc;
    auto body = qbuem::parse(doc, req.body());

    // .get("key") | fallback — 키 없거나 타입 불일치 시 nullopt 전파
    std::string msg  = body.get("message") | std::string("(no message)");
    std::string name = body.get("name")    | std::string("(anonymous)");

    qbuem::Document out_doc;
    auto out = qbuem::parse(out_doc, "{}");
    out.insert("echo_message", msg);
    out.insert("echo_name",    name);
    out.insert("ok", true);

    res.status(200)
       .header("Content-Type", "application/json")
       .body(out.dump());
    co_return;
}
```

CMake 통합:

```cmake
include(FetchContent)
FetchContent_Declare(qbuem_json
    GIT_REPOSITORY https://github.com/qbuem/qbuem-json.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(qbuem_json)

target_link_libraries(my_app PRIVATE draco::draco qbuem_json::qbuem_json)
```

---

### Server-Sent Events (SSE)

```cpp
#include <draco/middleware/sse.hpp>

app.get("/events", draco::Handler([](const draco::Request&, draco::Response& res) {
    draco::SseStream sse(res);
    sse.send("hello",  "message");          // event: message\ndata: hello\n\n
    sse.send("42",     "counter", "1");     // event + id 필드
    sse.heartbeat();                        // ": ping\n\n" — 연결 유지
    sse.close();                            // chunked 종료 마커 전송
}));
```

### gzip 압축 미들웨어

```cpp
#include <draco/middleware/compress.hpp>

// Accept-Encoding: gzip 를 보낸 클라이언트에만 자동 압축
// min_size 바이트 미만 응답은 압축 건너뜀 (기본값: 256)
app.use(draco::middleware::compress(512));
```

### JWT 인증 미들웨어 (HS256)

```cpp
#include <draco/middleware/jwt.hpp>

draco::middleware::JwtOptions jwt_opts;
jwt_opts.secret   = "my-secret-key";
jwt_opts.issuer   = "myapp";              // "iss" 클레임 검증 (선택)
jwt_opts.audience = "api";               // "aud" 클레임 검증 (선택)
// jwt_opts.verify_exp = true;           // exp 만료 검증 (기본값: true)

app.use(draco::middleware::jwt_verify(jwt_opts));

// 핸들러에서 검증된 sub 클레임 읽기
app.get("/profile", draco::Handler([](const draco::Request&, draco::Response& res) {
    // jwt_verify 미들웨어가 X-JWT-Sub 헤더로 전달
    res.status(200).body("{\"verified\":true}");
}));
```

### Unix Domain Socket

```cpp
// TCP 오버헤드 없이 로컬 IPC (리버스 프록시, 컨테이너)
auto result = app.listen_unix("/tmp/myapp.sock");
```

### StackController — 다중 App 통합 관리

```cpp
draco::App api_app, admin_app;

api_app.get("/api/v1/hello", draco::Handler([](const auto&, auto& res) {
    res.status(200).body("API");
}));
admin_app.get("/admin/status", draco::Handler([](const auto&, auto& res) {
    res.status(200).body("OK");
}));

draco::StackController ctrl;
ctrl.add(api_app,   8080);          // API 서버
ctrl.add(admin_app, 9090);          // 관리자 서버
ctrl.run();  // 블록; SIGTERM/SIGINT 자동 처리 → 양쪽 App 동시 종료
```

### CSRF 토큰 + constant-time 비교

```cpp
#include <draco/crypto.hpp>

// 생성 (128비트 엔트로피, URL-safe Base64)
auto token = draco::csrf_token();          // e.g. "dGhpcyBpcyBhIHRlc3Q"
res.set_cookie("csrf", token, {.same_site = "Strict", .http_only = false});

// 검증 (timing-attack 저항)
if (!draco::constant_time_equal(req.cookie("csrf"), expected)) {
    res.status(403).body("CSRF validation failed");
    return;
}
```

---

## 빌드

### 요구사항

| 항목 | 최소 버전 | 비고 |
|------|----------|------|
| **컴파일러** | GCC 13+ / Clang 16+ | C++20 필수 |
| **CMake** | 3.20+ | FetchContent 사용 |
| **OS** | Linux / macOS | Windows 미지원 |
| **Linux 커널** | 5.11+ (io_uring) | SQPOLL은 5.19+ 권장 |
| **macOS** | 12+ | Apple Silicon 포함 |

### 빌드 명령

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)            # Linux
make -j$(sysctl -n hw.ncpu) # macOS
```

### 빌드 옵션

| 옵션 | 기본값 | 설명 |
|------|--------|------|
| `DRACO_BUILD_TESTS` | ON | GTest 단위 테스트 빌드 |
| `DRACO_BUILD_EXAMPLES` | ON | 예제 바이너리 빌드 |
| `DRACO_INSTALL` | ON | CMake install 타겟 생성 |
| `DRACO_NO_EXCEPTIONS` | OFF | `-fno-exceptions` (zero-cost 모드) |
| `DRACO_NO_RTTI` | OFF | `-fno-rtti` |
| `DRACO_LTO` | OFF | Link-Time Optimization 활성화 |

### CMake 타겟

```cmake
find_package(draco REQUIRED)

target_link_libraries(my_app
    draco::draco   # 전체 라이브러리 (core + http)
)
# 또는 선택적으로:
# draco::core    — Reactor + Dispatcher
# draco::http    — HTTP 파서, Router, Request/Response
```

### 예제 실행

```bash
./examples/hello_world   # 기본 동기 핸들러
./examples/coro_json     # 비동기 코루틴 + qbuem-json (JSON)
./examples/async_timer   # AsyncSleep 타이머
```

---

## 성능 목표

| 지표 | 목표값 | 측정 방법 |
|------|--------|----------|
| **처리량** | 10M+ RPS (단일 서버) | `wrk -t$(nproc) -c1000` |
| **레이턴시 P99** | < 100µs (로컬) | `wrk2 --latency` |
| **동시 연결** | 10M (C10M) | 커스텀 스트레스 테스트 |
| **연결당 메모리** | < 1 KB | Arena 할당자 기반 |
| **Steady-state syscall** | 0 (io_uring SQPOLL) | `strace -c` 측정 |

---

## 지원 플랫폼

| 플랫폼 | 아키텍처 | 이벤트 모델 | CI 상태 |
|--------|---------|------------|---------|
| Linux | x86_64 | io_uring (epoll fallback) | ✅ |
| Linux | aarch64 | io_uring (epoll fallback) | ✅ |
| macOS | x86_64 | kqueue | ✅ |
| macOS | arm64 (Apple Silicon) | kqueue | ✅ |

---

## 알려진 제한사항 & 주의사항

**qbuem-json 사용 시:**
- `operator[]` 로 존재하지 않는 키에 접근하면 삽입되지 않음 → `.insert("key", val)` 사용
- 신뢰할 수 없는 데이터는 `.get("key")` (`SafeValue` 반환) 사용
- qbuem-json은 구 beast-json과 동일한 API를 제공하며 네임스페이스만 `qbuem::`으로 변경됨

**Reactor 콜백:**
- 동일 콜백 내에서 재귀적 이벤트 등록 시 상태 관리 주의

**플랫폼 제한:**
- Windows 미지원 (POSIX 전용 syscall 의존)
- io_uring SQPOLL은 `CAP_SYS_NICE` 또는 root 권한 필요 (Linux)
- macOS에서 최대 파일 디스크립터: `kern.maxfilesperproc` 조정 필요

---

## 로드맵

전체 개발 계획은 **[TODO.md](./TODO.md)** 를 참조하세요.

**Phase 5–10 (Critical):** io_uring 완성 · Zero-Cost C++ · HTTP/1.1 · 미들웨어 · 보안 · TLS
**Phase 11–15 (High):** HTTP/2 · HTTP/3/QUIC · WebSocket · Streaming · gRPC
**Phase 16–21 (Medium/Low):** 네트워크 심화 · Observability · 벤치마킹 · 테스트 · 문서화

---

## 기여

- 버그 리포트: 재현 가능한 C++ 예제와 함께 GitHub Issue 제출
- PR 전 필수: `clang-tidy` + `AddressSanitizer` + `ThreadSanitizer` 통과
- 성능 PR: `wrk` before/after 벤치마크 수치 첨부

---

*Created by The LKB Innovations.*
