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
| IPv6 dual-stack | ✅ 완료 | IPv4/IPv6 Dual-stack listen |
| TCP_FASTOPEN | ✅ 완료 | Linux listen socket |
| SO_NOSIGPIPE | ✅ 완료 | macOS client socket |
| Remote Addr | ✅ 완료 | `req.remote_addr()` 지원 |
| Crypto Utils | ✅ 완료 | constant-time compare, CSPRNG, CSRF token |
| Access Log | ✅ 완료 | Combined Log Format 지원 |
| JSON (코어 의존 없음) | ✅ 완료 | 프레임워크 코어에서 완전 제거; 앱이 직접 처리 |
| TLS/SSL | ❌ 예정 | Phase 8 |
| HTTP/2 | ❌ 예정 | Phase 9 |
| HTTP/3 / QUIC | ❌ 예정 | Phase 9 |
| WebSocket | ❌ 예정 | Phase 9 |
| gRPC | ❌ 예정 | Phase 9 |

---

## 빠른 시작

```cpp
#include <zenith/zenith.hpp>

int main() {
    auto controller = zenith::StackController::Create();

    // 동기 핸들러
    controller->Handle("/hello", [](const auto& req, auto& res) {
        res.status(200).body("Hello, Zenith!");
    });

    // 비동기 코루틴 핸들러
    controller->Handle("/user/:id", [](const auto& req, auto& res)
        -> zenith::Task<void> {
        auto id = req.param("id");
        res.status(200).body("{\"user_id\":\"" + std::string(id) + "\"}");
        co_return;
    });

    // 내장 엔드포인트 & 미들웨어
    controller->Use(zenith::middleware::request_id());
    controller->HealthCheck("/health");
    controller->Metrics("/metrics");

    return controller->Run(8080);
}
```


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
           .body("Hello, Draco!");
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

---

## 빌드

### 요구사항

| 항목 | 최소 버전 | 비고 |
|------|----------|------|
| **컴파일러** | GCC 13+ / Clang 16+ | C++23 필수 |
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
./examples/coro_json     # 비동기 코루틴 + JSON
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

**Beast JSON 사용 시:**
- `operator[]` 로 존재하지 않는 키에 접근하면 삽입되지 않음 → `.insert("key", val)` 사용
- 신뢰할 수 없는 데이터는 `.get("key")` (`SafeValue` 반환) 사용

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
