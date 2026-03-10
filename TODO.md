# Draco WAS Development Roadmap (v0.2.0 → 1.0.0)

Draco WAS is a C++23 ultra-low latency Web Application Server designed for extreme performance and zero-dependency agility.

> **표기 규칙**
> - `[Common]` — Linux / macOS 공통 작업
> - `[Linux]`  — Linux 전용 (io_uring / epoll / sendfile 등)
> - `[macOS]`  — macOS 전용 (kqueue / GCD 등)

---

## ✅ Phase 0–2: Foundation & Core Engine (완료)

- [x] `[Common]` Expert architecture design (Shared-Nothing, io_uring, Coroutines)
- [x] `[Common]` C++23 project foundation & directory structure
- [x] `[Common]` Dependency management via `FetchContent` (Beast-JSON, GTest)
- [x] `[Linux]`  **Reactor Core**: `epoll` reactor 구현
- [x] `[macOS]`  **Reactor Core**: `kqueue` reactor 구현
- [x] `[Common]` **Dispatcher**: Thread-per-core Dispatcher with CPU affinity
- [x] `[Common]` **HTTP Abstractions**: Zero-copy HTTP Parser, `Request`/`Response` API
- [x] `[Common]` **Modern Routing**: Radix Tree for fast route matching
- [x] `[Common]` **Library Infrastructure**: Header + source distribution (modern CMake)

## ✅ Phase 3: Coroutines & Beast JSON (완료)

- [x] `[Common]` Custom `draco::Task<T>` with symmetric transfer for nested coroutines
- [x] `[Common]` Beast JSON native integration in `Request`/`Response`
- [x] `[Common]` `SafeValue` / `.get()` pattern for untrusted JSON data

## ✅ Phase 4: Async I/O & Hardening (완료)

- [x] `[Common]` Multi-event support (Read/Write) per FD
- [x] `[Common]` `AsyncRead`, `AsyncWrite`, `AsyncSleep` awaiters
- [x] `[Linux]`  `EpollReactor` with `epoll` + `timerfd`
- [x] `[Common]` Callback safety — copy-before-invoke, preventing UAF
- [x] `[Common]` `Task::detach()` fire-and-forget coroutine lifecycle
- [x] `[Common]` Async handler hardening with corrected `detach()` semantics
- [x] `[Common]` Platform-specific conditional compilation (kqueue / epoll)
- [x] `[Common]` Test suite: `http_test.cpp`, `reactor_test.cpp`

---

## 🔵 Phase 5: io_uring 완성 & 네이티브 Zero-Copy I/O

> 목표: Linux에서 syscall overhead 를 제로에 가깝게 줄이기

### Linux 전용

- [ ] `[Linux]`  **io_uring Reactor 완성**: `io_uring_reactor.cpp` stub → 실제 구현
  - [ ] `io_uring_setup` / `io_uring_enter` 기반 event loop
  - [ ] SQE/CQE 배치 처리 (Submission Queue Batching)
  - [ ] `IORING_OP_READ`, `IORING_OP_WRITE`, `IORING_OP_ACCEPT` 지원
  - [ ] `IORING_OP_TIMEOUT` 으로 `AsyncSleep` 구현
  - [ ] Fixed buffers (`io_uring_register_buffers`) 로 zero-copy read
  - [ ] Kernel 5.19+ `IORING_FEAT_CQE_SKIP` 활용
  - [ ] `SO_REUSEPORT` + `IORING_OP_ACCEPT_DIRECT` 다중 수신 소켓
- [ ] `[Linux]`  **`sendfile(2)`** — 정적 파일 서빙 zero-copy 전송
- [ ] `[Linux]`  **`splice(2)`** — pipe 경유 소켓→소켓 zero-copy 프록시
- [ ] `[Linux]`  **`SO_REUSEPORT`** 로 멀티코어 Accept 분산 (accept storm 제거)

### macOS 전용

- [ ] `[macOS]`  **`kqueue` Timer 정밀도 개선**: `EVFILT_TIMER` ms→µs 해상도
- [ ] `[macOS]`  **`sendfile(2)` macOS variant**: `off_t *len` 시그니처 처리
- [ ] `[macOS]`  **`EVFILT_USER`** 기반 wakeup (cross-thread notification)

### 공통

- [ ] `[Common]` Reactor 추상 인터페이스에 `batch_submit()` / `batch_wait()` 추가
- [ ] `[Common]` `AsyncAccept` awaiter — accept loop 비동기화

---

## 🔵 Phase 6: HTTP/1.1 완전 준수

> 목표: RFC 7230/7231/7234 완전 지원

- [ ] `[Common]` **Keep-Alive / Connection Reuse**
  - [ ] `Connection: keep-alive` 헤더 파싱 및 응답
  - [ ] keep-alive timeout & max-requests per connection 설정
  - [ ] per-connection state machine (Idle → Reading → Processing → Writing → Idle)
- [ ] `[Common]` **Chunked Transfer Encoding** (송수신 모두)
  - [ ] 청크 파싱 (요청 body)
  - [ ] 청크 직렬화 (응답 body streaming)
- [ ] `[Common]` **Pipelining** — 순서 보장 응답 큐 (head-of-line 유지)
- [ ] `[Common]` **Content-Length 자동 계산** — 응답 직렬화 시 자동 삽입
- [ ] `[Common]` **100 Continue** 처리 (`Expect: 100-continue`)
- [ ] `[Common]` **Request body 크기 제한** — configurable `max_body_size`
- [ ] `[Common]` **Connection Timeout**
  - [ ] Idle timeout (keep-alive 유휴 시간)
  - [ ] Read timeout (헤더 수신 최대 시간)
  - [ ] Write timeout (응답 전송 최대 시간)
- [ ] `[Common]` **Graceful Shutdown** — 진행 중 요청 완료 후 종료
  - [ ] `SIGTERM` / `SIGINT` 핸들러 등록
  - [ ] drain 모드: 신규 accept 중단, 기존 연결 완료 대기
- [ ] `[Common]` **`Date` 헤더** — cached 포맷 (1초 단위 갱신)
- [ ] `[Common]` **Conditional Requests** — `ETag`, `Last-Modified`, `If-None-Match`, `If-Modified-Since`
- [ ] `[Common]` **Range Requests** — `Range` 헤더, 206 Partial Content

---

## 🔵 Phase 7: 핵심 미들웨어 파이프라인

> 목표: 프로덕션 필수 미들웨어 내장

### 공통 미들웨어

- [ ] `[Common]` **미들웨어 체인 개선**
  - [ ] `next()` 기반 비동기 미들웨어 (`AsyncMiddleware`)
  - [ ] 에러 미들웨어 (`ErrorHandler` 4-arg variant)
  - [ ] 미들웨어 실행 순서 보장 (등록 순서)
- [ ] `[Common]` **Request ID 미들웨어** — UUID v4 자동 생성, `X-Request-ID` 헤더
- [ ] `[Common]` **Logging 미들웨어**
  - [ ] Combined Log Format (Apache/Nginx 호환)
  - [ ] JSON structured log 옵션
  - [ ] 응답 시간 측정 (ns 단위)
- [ ] `[Common]` **CORS 미들웨어**
  - [ ] `Access-Control-Allow-Origin` / `Methods` / `Headers` 설정
  - [ ] Preflight (`OPTIONS`) 자동 처리
  - [ ] 동적 Origin 화이트리스트 검사
- [ ] `[Common]` **Body Parser 미들웨어**
  - [ ] `application/json` → beast-json 파싱
  - [ ] `application/x-www-form-urlencoded` 파싱
  - [ ] `multipart/form-data` 파싱 (파일 업로드)
- [ ] `[Common]` **Rate Limiting 미들웨어**
  - [ ] 토큰 버킷 알고리즘 (thread-local per reactor)
  - [ ] IP 기반 / API key 기반 제한
  - [ ] `X-RateLimit-*` 응답 헤더
- [ ] `[Common]` **Compression 미들웨어**
  - [ ] `Content-Encoding: gzip` (zlib)
  - [ ] `Content-Encoding: br` (brotli, optional)
  - [ ] `Accept-Encoding` 협상
- [ ] `[Common]` **Cookie 파싱** — `Cookie:` 헤더 파싱, `Set-Cookie:` 응답 빌더
- [ ] `[Common]` **Session 미들웨어** — in-memory 세션 맵 (thread-safe)
- [ ] `[Common]` **JWT 미들웨어**
  - [ ] HS256 / RS256 검증
  - [ ] `Authorization: Bearer <token>` 파싱
- [ ] `[Common]` **Static File Serving 미들웨어**
  - [ ] MIME type 자동 감지
  - [ ] `ETag` / `Last-Modified` 자동 생성
  - [ ] Directory listing (optional)
- [ ] `[Common]` **Health Check** — `GET /health` → `{"status":"ok"}` 내장 핸들러
- [ ] `[Common]` **Panic Recovery 미들웨어** — 핸들러 예외 catch → 500 응답 변환

---

## 🔵 Phase 8: TLS/SSL 지원

> 목표: HTTPS 완전 지원 (OpenSSL / BoringSSL)

### 공통

- [ ] `[Common]` **TLS 추상 레이어** — `TlsStream` wrapper (openssl/boringssl 양쪽 지원)
- [ ] `[Common]` **TLS 핸드셰이크 비동기화** — reactor 이벤트 기반 non-blocking handshake
- [ ] `[Common]` **인증서 설정 API**
  - [ ] `app.tls({ cert, key, ca })` 설정
  - [ ] PEM / DER 포맷 지원
  - [ ] Hot reload (SIGUSR1 트리거) — 다운타임 없는 인증서 교체
- [ ] `[Common]` **SNI 지원** — 다중 도메인 인증서 (Server Name Indication)
- [ ] `[Common]` **mTLS** — 클라이언트 인증서 검증 (optional)
- [ ] `[Common]` **ALPN 협상** — `http/1.1` / `h2` 프로토콜 선택
- [ ] `[Common]` **Session Resumption** — TLS 1.3 0-RTT / Session Tickets
- [ ] `[Common]` **OCSP Stapling** — 인증서 유효성 캐시

### Linux 전용

- [ ] `[Linux]`  **`KTLS` (Kernel TLS)** — `setsockopt(TCP_ULP, "tls")` zero-copy 암호화

---

## 🔵 Phase 9: HTTP/2 지원

> 목표: nghttp2 기반 HTTP/2 완전 구현

- [ ] `[Common]` **nghttp2 통합** — FetchContent 또는 vcpkg
- [ ] `[Common]` **Connection Preface** — 클라이언트 Magic + SETTINGS 프레임 처리
- [ ] `[Common]` **HPACK 헤더 압축** — nghttp2 내장 HPACK 활용
- [ ] `[Common]` **스트림 다중화** — 단일 TCP 연결 위 다중 요청/응답 스트림
- [ ] `[Common]` **Flow Control** — 스트림 / 연결 레벨 윈도우 관리
- [ ] `[Common]` **Server Push** — `app.push(path, headers)` API
- [ ] `[Common]` **스트림 우선순위** — `PRIORITY` 프레임 처리
- [ ] `[Common]` **`GOAWAY` / `RST_STREAM`** 정상 처리
- [ ] `[Common]` **HTTP/2 → HTTP/1.1 Upgrade** — `h2c` cleartext 업그레이드
- [ ] `[Common]` **ALPN 통합** — Phase 8 TLS ALPN과 연동하여 h2 자동 선택

---

## 🔵 Phase 10: WebSocket 지원

> 목표: RFC 6455 완전 준수 WebSocket

- [ ] `[Common]` **Upgrade 핸드셰이크** — `101 Switching Protocols` 처리
- [ ] `[Common]` **프레임 파서** — FIN, opcode, mask, payload 처리
- [ ] `[Common]` **프레임 직렬화** — 서버→클라이언트 마스킹 없이 전송
- [ ] `[Common]` **opcode 처리**
  - [ ] `0x1` Text frame
  - [ ] `0x2` Binary frame
  - [ ] `0x8` Close frame (정상 종료)
  - [ ] `0x9` / `0xA` Ping / Pong (자동 응답)
- [ ] `[Common]` **단편화 (Fragmentation)** — 멀티 프레임 메시지 재조립
- [ ] `[Common]` **메시지 크기 제한** — `max_message_size` 설정
- [ ] `[Common]` **`WebSocket` API**
  - [ ] `ws.send(text)` / `ws.send(binary)`
  - [ ] `ws.close(code, reason)`
  - [ ] `ws.on_message(handler)` / `ws.on_close(handler)`
- [ ] `[Common]` **Broadcast 지원** — 연결 레지스트리 + 멀티캐스트 전송
- [ ] `[Common]` **WS over TLS** (wss://) — Phase 8 TLS 통합
- [ ] `[Common]` **Subprotocol 협상** — `Sec-WebSocket-Protocol` 헤더

---

## 🔵 Phase 11: Streaming & SSE (Server-Sent Events)

- [ ] `[Common]` **Response Streaming API** — `res.write(chunk)` + `res.end()`
- [ ] `[Common]` **Server-Sent Events (SSE)**
  - [ ] `text/event-stream` 콘텐츠 타입 응답
  - [ ] `SseStream` 헬퍼: `send_event(name, data, id)` API
  - [ ] heartbeat (`comment:` ping) 자동 전송
- [ ] `[Common]` **파일 스트리밍** — 대용량 파일 청크 전송 (메모리에 올리지 않음)
- [ ] `[Common]` **백프레셔 (Backpressure)** — 소켓 쓰기 버퍼 포화 시 일시정지/재개
- [ ] `[Common]` **Multipart 응답** — `multipart/x-mixed-replace` (MJPEG 스트림 등)

---

## 🔵 Phase 12: 네트워크 심화 기능

### 공통

- [ ] `[Common]` **IPv6 완전 지원** — `::1` 바인딩, dual-stack `IPV6_V6ONLY` 설정
- [ ] **Unix Domain Socket** — `app.listen("/tmp/draco.sock")` 지원
  - [ ] `[Linux]`  `AF_UNIX` SOCK_STREAM
  - [ ] `[macOS]`  `AF_UNIX` SOCK_STREAM
- [ ] `[Common]` **리버스 프록시 지원**
  - [ ] `X-Forwarded-For` / `X-Real-IP` 헤더 파싱
  - [ ] Trusted proxy 화이트리스트 설정
- [ ] `[Common]` **Virtual Hosting** — Host 헤더 기반 라우터 분기
- [ ] `[Common]` **HTTP 클라이언트 (비동기)** — 백엔드 API 호출용 내장 클라이언트
  - [ ] `co_await client.get(url, headers)`
  - [ ] Keep-alive pool (per-domain connection reuse)
- [ ] `[Common]` **DNS 조회 (비동기)** — getaddrinfo 블로킹 해결 (thread pool offload)

### Linux 전용

- [ ] `[Linux]`  **`TCP_FASTOPEN`** — 3-way handshake 없이 첫 패킷에 데이터 전송
- [ ] `[Linux]`  **`TCP_NODELAY`** / **`TCP_CORK`** 세밀 튜닝 API
- [ ] `[Linux]`  **`SO_INCOMING_CPU`** — RSS 기반 CPU 친화성 소켓 배분
- [ ] `[Linux]`  **NUMA 인식 스레드 배치** — `numa_alloc_onnode` / `set_mempolicy`

### macOS 전용

- [ ] `[macOS]`  **`TCP_FASTOPEN`** — macOS 12+ 지원 (`setsockopt` 옵션 확인)
- [ ] `[macOS]`  **`SO_NOSIGPIPE`** — SIGPIPE 억제 (macOS 전용 소켓 옵션)
- [ ] `[macOS]`  **Grand Central Dispatch (GCD) 연동** — 선택적 GCD 기반 타이머

---

## 🔵 Phase 13: URL / Request 유틸리티

- [ ] `[Common]` **URL 파싱** — path, query string, fragment 완전 분리
- [ ] `[Common]` **Query String 파싱** — `?foo=bar&baz=1` → `req.query("foo")` API
- [ ] `[Common]` **URL 인코딩/디코딩** — percent-encoding 유틸리티
- [ ] `[Common]` **Path Normalization** — `..` / `.` 제거, 이중 슬래시 처리
- [ ] `[Common]` **MIME Type 레지스트리** — 확장자 → Content-Type 자동 매핑
- [ ] `[Common]` **Content Negotiation** — `Accept:` 헤더 q-factor 파싱 및 협상
- [ ] `[Common]` **`req.ip()`** — 실제 클라이언트 IP 추출 (프록시 헤더 포함)
- [ ] `[Common]` **Multipart Form Data 파서** — 바이너리 파일 업로드 포함

---

## 🔵 Phase 14: 성능 & 벤치마킹

### 공통

- [ ] `[Common]` **내장 벤치마크 스위트**
  - [ ] `wrk` / `hey` / `k6` 스크립트 제공
  - [ ] Latency P50 / P95 / P99 / P99.9 측정
  - [ ] Throughput (RPS) 측정 자동화
- [ ] `[Common]` **C10M 스트레스 테스트**
  - [ ] 10M 동시 연결 목표 시나리오
  - [ ] 연결당 메모리 사용량 측정
- [ ] `[Common]` **Flame Graph 통합** — 프로파일링 가이드 (perf / Instruments)
- [ ] `[Common]` **Zero-copy Serialization 최적화**
  - [ ] 응답 헤더 iovec scatter-gather 직렬화
  - [ ] Beast JSON write buffer chain 최적화
- [ ] `[Common]` **Lock-free 데이터 구조** — 연결 레지스트리 / 라우팅 테이블 원자적 갱신
- [ ] `[Common]` **메모리 최적화**
  - [ ] Arena 할당자 slab 사이즈 튜닝
  - [ ] per-reactor 객체 풀 (Request / Response 재사용)

### Linux 전용

- [ ] `[Linux]`  **Huge Pages** — `mmap(MAP_HUGETLB)` 기반 arena 페이지
- [ ] `[Linux]`  **`perf stat`** 통합 테스트 — cache miss / branch mispred 자동 리포트
- [ ] `[Linux]`  **`io_uring` 배치 최적화** — SQ polling (`IORING_SETUP_SQPOLL`)

### macOS 전용

- [ ] `[macOS]`  **Instruments / DTrace 프로파일링 가이드**
- [ ] `[macOS]`  **`kqueue` kevent 배치 처리** — `nevents` 최대화 튜닝

---

## 🔵 Phase 15: 테스트 & CI/CD 강화

- [ ] `[Common]` **통합 테스트 확장**
  - [ ] Keep-alive 커넥션 재사용 테스트
  - [ ] Chunked 인코딩 수신/송신 테스트
  - [ ] 미들웨어 체인 순서 테스트
  - [ ] 에러 핸들러 테스트
  - [ ] Concurrent 요청 경쟁 조건 테스트
- [ ] `[Common]` **Fuzzing** — libFuzzer 기반 HTTP 파서 퍼징
- [ ] `[Common]` **Coverage Report** — lcov / gcovr + HTML 리포트
- [ ] `[Common]` **AddressSanitizer / UBSan / ThreadSanitizer** CI 통합
- [ ] `[Common]` **Valgrind** 메모리 누수 검사 (Linux CI)
- [ ] `[Linux]`  **Valgrind / Helgrind** race condition 검사
- [ ] `[macOS]`  **Leaks / MallocStackLogging** 메모리 누수 검사
- [ ] `[Common]` **GitHub Actions 매트릭스 확장**
  - [ ] `ubuntu-22.04` + `ubuntu-24.04` (GCC 13, Clang 17)
  - [ ] `macos-13` (x86_64) + `macos-14` (Apple Silicon arm64)
  - [ ] Nightly 벤치마크 자동화 (PR 대비 성능 회귀 감지)

---

## 🔵 Phase 16: 문서화 & DX (Developer Experience)

- [ ] `[Common]` **API 레퍼런스 문서** — Doxygen + GitHub Pages 자동 배포
- [ ] `[Common]` **Getting Started 가이드** — 5분 안에 Hello World 실행
- [ ] `[Common]` **예제 확장**
  - [ ] REST API 서버 (CRUD + JWT 인증)
  - [ ] WebSocket 채팅 서버
  - [ ] SSE 실시간 피드
  - [ ] 정적 파일 서버 (SPA 호스팅)
  - [ ] TLS HTTPS 서버
  - [ ] HTTP/2 서버
  - [ ] 리버스 프록시 서버
- [ ] `[Common]` **Migration Guide** — v0.x → v1.0 Breaking Change 문서
- [ ] `[Common]` **성능 튜닝 가이드**
  - [ ] `[Linux]`  io_uring 커널 파라미터 (ulimit, `nr_open`, `memlock`)
  - [ ] `[macOS]`  kqueue / `kern.maxfilesperproc` 설정 가이드
- [ ] `[Common]` **아키텍처 다이어그램** — Reactor / Dispatcher / Handler 흐름도 (Mermaid)

---

## 📌 Phase 우선순위 요약

| Phase | 핵심 내용 | 플랫폼 | 중요도 |
|-------|----------|--------|--------|
| **5** | io_uring 완성 + Zero-Copy I/O | Linux / macOS | 🔴 Critical |
| **6** | HTTP/1.1 완전 준수 (Keep-Alive, Chunked, Timeout) | Common | 🔴 Critical |
| **7** | 핵심 미들웨어 (CORS, Body Parser, Rate Limit, Logging) | Common | 🔴 Critical |
| **8** | TLS/SSL (HTTPS, SNI, ALPN) | Common + Linux | 🔴 Critical |
| **9** | HTTP/2 (nghttp2) | Common | 🟠 High |
| **10** | WebSocket (RFC 6455) | Common | 🟠 High |
| **11** | Streaming / SSE | Common | 🟠 High |
| **12** | 네트워크 심화 (IPv6, UDS, TCP 튜닝) | Common + Linux + macOS | 🟡 Medium |
| **13** | URL / Request 유틸리티 | Common | 🟡 Medium |
| **14** | 성능 & 벤치마킹 | Common + Linux + macOS | 🟡 Medium |
| **15** | 테스트 & CI/CD | Common + Linux + macOS | 🟡 Medium |
| **16** | 문서화 & DX | Common | 🟢 Low |

---

## 📝 Contribution & Issues

- 버그는 재현 가능한 C++ 예제와 함께 GitHub Issue 로 제출
- 성능 프로파일링: `[Linux]` `perf` / `[macOS]` Instruments
- PR 제출 전 `clang-tidy` 및 `AddressSanitizer` 통과 필수

---

*Created by The LKB Innovations.*
