# Draco WAS Development Roadmap (v0.2.0 → 1.0.0)

Draco WAS는 **Zero Latency · Zero Cost · Low Memory · Low CPU** 를 4대 핵심 원칙으로 삼는 C++23 초고성능 Web Application Server 라이브러리입니다.

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

## 🔴 Phase 5: io_uring 완성 & 네이티브 Zero-Copy I/O

> 목표: Linux syscall overhead를 정상 상태(steady-state)에서 **0**으로 줄이기

### Linux 전용

- [ ] `[Linux]`  **io_uring Reactor 완성**: `io_uring_reactor.cpp` stub → 실제 구현
  - [ ] `io_uring_setup` / `io_uring_enter` 기반 event loop
  - [ ] SQE/CQE 배치 처리 (Submission Queue Batching)
  - [ ] `IORING_OP_READ`, `IORING_OP_WRITE`, `IORING_OP_ACCEPT` 지원
  - [ ] `IORING_OP_TIMEOUT` 으로 `AsyncSleep` 구현
  - [ ] **Fixed buffers** (`io_uring_register_buffers`) — kernel이 직접 user buffer에 DMA write
  - [ ] **Buffer Ring** (`IORING_OP_PROVIDE_BUFFERS`) — kernel이 버퍼 선택, copy 제로
  - [ ] **SQPOLL 모드** (`IORING_SETUP_SQPOLL`) — kernel thread가 SQ 폴링, 정상 상태 syscall 0
  - [ ] Kernel 5.19+ `IORING_FEAT_CQE_SKIP` 활용
  - [ ] `SO_REUSEPORT` + `IORING_OP_ACCEPT_DIRECT` 다중 수신 소켓
- [ ] `[Linux]`  **`MSG_ZEROCOPY`** (`SO_ZEROCOPY`) — 송신 시 kernel→user 복사 제거, CQE 완료 알림
- [ ] `[Linux]`  **`sendfile(2)`** — 정적 파일 서빙 zero-copy 전송
- [ ] `[Linux]`  **`splice(2)`** — pipe 경유 소켓→소켓 zero-copy 프록시
- [ ] `[Linux]`  **`SO_REUSEPORT`** + BPF 소켓 배분 — accept storm 제거
- [ ] `[Linux]`  **`SO_BUSY_POLL`** / **`SO_PREFER_BUSY_POLL`** — 인터럽트 없이 busy-poll로 수신 지연 제거
- [ ] `[Linux]`  **AF_XDP** (eXpress Data Path) — 커널 네트워크 스택 bypass 옵션 (극단적 성능 필요 시)

### macOS 전용

- [ ] `[macOS]`  **`kqueue` Timer 정밀도 개선**: `EVFILT_TIMER` ms→µs 해상도
- [ ] `[macOS]`  **`sendfile(2)` macOS variant**: `off_t *len` 시그니처 처리
- [ ] `[macOS]`  **`EVFILT_USER`** 기반 wakeup (cross-thread notification)

### 공통

- [ ] `[Common]` Reactor 추상 인터페이스에 `batch_submit()` / `batch_wait()` 추가
- [ ] `[Common]` **`AsyncAccept` awaiter** — accept loop 완전 비동기화
- [ ] `[Common]` **`writev` / `sendmsg` scatter-gather** — 헤더 + body를 단일 syscall로 전송

---

## 🔴 Phase 6: Zero-Cost C++ Architecture

> 목표: 런타임 오버헤드 **0** — 모든 추상화는 컴파일 타임에 제거

### 컴파일 타임 최적화

- [ ] `[Common]` **CRTP 기반 정적 다형성** — 핫 패스에서 `virtual` 디스패치 제거
  - [ ] `Reactor<Derived>` CRTP 기반 리팩토링 — vtable 비용 제거
  - [ ] `Handler<Derived>` 정적 핸들러 체인
- [ ] `[Common]` **컴파일 타임 라우팅** — `consteval` / `constexpr` 라우팅 테이블 구축
  - [ ] 경로 패턴을 컴파일 시 파싱 → 런타임 비교 비용 최소화
  - [ ] 고정 경로(`/health`, `/metrics`)는 완전 컴파일 타임 resolve
- [ ] `[Common]` **Policy-based Design** — 로깅/직렬화/할당자 전략을 템플릿 파라미터로 교체
- [ ] `[Common]` **`[[always_inline]]` 어노테이션** — 핫 패스 함수 강제 인라인
- [ ] `[Common]` **`[[likely]]` / `[[unlikely]]`** — 분기 예측 힌트 전면 적용
- [ ] `[Common]` **`-fno-exceptions`** 빌드 옵션 지원 — 예외 핸들링 overhead 제거
- [ ] `[Common]` **`-fno-rtti`** 빌드 옵션 지원 — RTTI 테이블 메모리 제거

### 링크 타임 / 바이너리 최적화

- [ ] `[Common]` **LTO (Link-Time Optimization)** — `-flto=thin` whole-program 인라이닝
- [ ] `[Common]` **PGO (Profile-Guided Optimization)** 2-pass 빌드 가이드
  - [ ] Instrumented build → wrk 프로파일 수집 → Optimized build
- [ ] `[Linux]`  **BOLT** (Binary Optimization and Layout Tool) — 핫 함수 재배치로 I-cache miss 감소
- [ ] `[macOS]`  **`-fprofile-instr-generate`** Clang PGO 지원

### CPU 레벨 최적화

- [ ] `[Common]` **SIMD 가속 HTTP 파서** — AVX2 (x86) / NEON (ARM) 벡터 연산으로 헤더 스캐닝
  - [ ] 개행(`\r\n`) 탐색: 16바이트 병렬 비교 (picohttpparser 기법 참고)
  - [ ] Method / 상태코드 매칭: SIMD 문자열 비교
- [ ] `[Common]` **Cache Line 정렬** — 핫 데이터 구조에 `alignas(64)` 적용
  - [ ] Reactor, Connection, Arena 헤더를 캐시라인에 맞게 패킹
  - [ ] `std::hardware_destructive_interference_size` 로 false sharing 방지
- [ ] `[Common]` **프리패치 힌트** — `__builtin_prefetch` 로 다음 Connection 구조체 미리 로드
- [ ] `[Common]` **소규모 버퍼 스택 할당** — 2KB 이하 요청 헤더는 힙 대신 스택 사용
- [ ] `[Common]` **SSO (Small String Optimization)** — 짧은 헤더값 힙 할당 없이 인라인 저장
- [ ] `[Common]` **vDSO 타이밍** — `clock_gettime(CLOCK_MONOTONIC_COARSE)` 로 syscall 없는 타임스탬프

### 메모리 서브시스템

- [ ] `[Common]` **Slab 할당자** — Connection / Request / Response 오브젝트 풀 (free-list 재사용)
- [ ] `[Common]` **tcmalloc / jemalloc 통합 옵션** — 전역 malloc 교체 빌드 플래그
- [ ] `[Common]` **헤더 문자열 인터닝** — `Content-Type`, `Content-Length` 등 상용 헤더 포인터 재사용
- [ ] `[Linux]`  **`madvise(MADV_HUGEPAGE)`** — arena 메모리 Transparent Huge Page 활성화
- [ ] `[Linux]`  **`mmap(MAP_HUGETLB)`** — 명시적 2MB 페이지로 TLB 미스 감소
- [ ] `[macOS]`  **`VM_FLAGS_SUPERPAGE_SIZE_2MB`** — macOS Huge Page 매핑

---

## 🔴 Phase 7: HTTP/1.1 완전 준수

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
- [ ] `[Common]` **`Date` 헤더** — atomic cached 포맷 (1초 단위 갱신, lock-free)
- [ ] `[Common]` **Conditional Requests** — `ETag`, `Last-Modified`, `If-None-Match`, `If-Modified-Since`
- [ ] `[Common]` **Range Requests** — `Range` 헤더, 206 Partial Content
- [ ] `[Common]` **`TCP_QUICKACK`** — ACK 즉시 전송으로 RTT 단축
- [ ] `[Common]` **`TCP_DEFER_ACCEPT`** — 데이터 도착 후 accept() 실행 (SYN flood 방어 겸용)
- [ ] `[Common]` **`TCP_CORK` / `MSG_MORE`** — 헤더+body 단일 전송 배칭

---

## 🔴 Phase 8: 핵심 미들웨어 파이프라인

> 목표: 프로덕션 필수 미들웨어 — 모두 zero-allocation으로 구현

### 미들웨어 엔진

- [ ] `[Common]` **미들웨어 체인 개선**
  - [ ] `next()` 기반 비동기 미들웨어 (`AsyncMiddleware`)
  - [ ] 에러 미들웨어 (`ErrorHandler` 4-arg variant)
  - [ ] 미들웨어 실행 순서 보장 (등록 순서)
  - [ ] 미들웨어 체인을 컴파일 타임에 flatten (zero-overhead pipeline)

### 필수 미들웨어

- [ ] `[Common]` **Request ID 미들웨어** — UUID v4 자동 생성 (lock-free), `X-Request-ID` 헤더
- [ ] `[Common]` **Logging 미들웨어**
  - [ ] Combined Log Format (Apache/Nginx 호환)
  - [ ] JSON structured log 옵션
  - [ ] 응답 시간 측정 (ns 단위, `CLOCK_MONOTONIC`)
  - [ ] **비동기 로그 링 버퍼** — 로그를 ring buffer에 enqueue, 별도 thread가 flush (hot path 블로킹 없음)
- [ ] `[Common]` **CORS 미들웨어**
  - [ ] `Access-Control-Allow-Origin` / `Methods` / `Headers` 설정
  - [ ] Preflight (`OPTIONS`) 자동 처리
  - [ ] 동적 Origin 화이트리스트 검사
- [ ] `[Common]` **Body Parser 미들웨어**
  - [ ] `application/json` → beast-json 파싱
  - [ ] `application/x-www-form-urlencoded` 파싱
  - [ ] `multipart/form-data` 파싱 (파일 업로드)
- [ ] `[Common]` **Rate Limiting 미들웨어**
  - [ ] 토큰 버킷 알고리즘 (thread-local per reactor, lock-free)
  - [ ] IP 기반 / API key 기반 제한
  - [ ] `X-RateLimit-*` 응답 헤더
- [ ] `[Common]` **Compression 미들웨어**
  - [ ] `Content-Encoding: gzip` (zlib, streaming 압축)
  - [ ] `Content-Encoding: br` (brotli, optional)
  - [ ] `Accept-Encoding` 협상 (q-factor 파싱)
- [ ] `[Common]` **Cookie 파싱** — `Cookie:` 헤더 파싱, `Set-Cookie:` 응답 빌더
- [ ] `[Common]` **Session 미들웨어** — lock-free 세션 맵 (per-reactor 분산)
- [ ] `[Common]` **JWT 미들웨어**
  - [ ] HS256 / RS256 검증
  - [ ] `Authorization: Bearer <token>` 파싱
  - [ ] **상수 시간 서명 비교** (`CRYPTO_memcmp`) — timing attack 방지
- [ ] `[Common]` **Static File Serving 미들웨어**
  - [ ] MIME type 자동 감지
  - [ ] `ETag` / `Last-Modified` 자동 생성
  - [ ] Directory listing (optional)
- [ ] `[Common]` **Health Check** — `GET /health` → `{"status":"ok"}` 내장 핸들러 (컴파일 타임 상수 응답)
- [ ] `[Common]` **Panic Recovery 미들웨어** — 핸들러 예외 catch → 500 응답 변환

---

## 🔴 Phase 9: 보안 하드닝

> 목표: 프로덕션 보안 필수 항목 — 성능 페널티 없이 방어

### 프로토콜 레벨 방어

- [ ] `[Common]` **HTTP Request Smuggling 방지**
  - [ ] `Transfer-Encoding` + `Content-Length` 동시 존재 시 400 거부
  - [ ] 모호한 청크 헤더 파싱 엄격 모드 (`chunked` 외 무시)
- [ ] `[Common]` **HTTP Header Injection 방지** — 헤더값 `\r\n` 포함 시 reject
- [ ] `[Common]` **Slowloris 공격 완화** — Read timeout (Phase 7) + 헤더 최대 크기 제한 조합
- [ ] `[Common]` **Request Flood 방지** — Rate Limiting (Phase 8) + per-IP connection 수 제한
- [ ] `[Common]` **Path Traversal 방지** — `../`, `%2e%2e` URL 정규화 후 거부
- [ ] `[Common]` **Large Header Bomb 방지** — 단일 헤더 최대 크기 제한 (configurable, 기본 8KB)

### 보안 헤더 헬퍼

- [ ] `[Common]` **HSTS** — `Strict-Transport-Security` 헬퍼
- [ ] `[Common]` **CSP** — `Content-Security-Policy` 빌더 API
- [ ] `[Common]` **X-Frame-Options** / **X-Content-Type-Options** / **Referrer-Policy** 헬퍼
- [ ] `[Common]` **Permissions-Policy** 헤더 빌더

### 암호화

- [ ] `[Common]` **상수 시간 문자열 비교** — timing attack 방지 (`crypto_memcmp` 유틸)
- [ ] `[Common]` **CSRF 토큰 생성** — `CSPRNG`(`getrandom`/`arc4random`) 기반 안전 난수
- [ ] `[Linux]`  **`getrandom(2)` syscall** — `/dev/urandom` 파일 open 없이 난수 획득
- [ ] `[macOS]`  **`arc4random_buf()`** — macOS 커널 제공 CSPRNG

---

## 🔴 Phase 10: TLS/SSL 지원

> 목표: HTTPS 완전 지원 (OpenSSL / BoringSSL) — non-blocking, zero-copy

### 공통

- [ ] `[Common]` **TLS 추상 레이어** — `TlsStream` wrapper (openssl/boringssl 양쪽 지원)
- [ ] `[Common]` **TLS 핸드셰이크 비동기화** — reactor 이벤트 기반 non-blocking handshake
- [ ] `[Common]` **인증서 설정 API**
  - [ ] `app.tls({ cert, key, ca })` 설정
  - [ ] PEM / DER 포맷 지원
  - [ ] Hot reload (SIGUSR1 트리거) — 다운타임 없는 인증서 교체
- [ ] `[Common]` **SNI 지원** — 다중 도메인 인증서 (Server Name Indication)
- [ ] `[Common]` **mTLS** — 클라이언트 인증서 검증 (optional)
- [ ] `[Common]` **ALPN 협상** — `http/1.1` / `h2` / `h3` 프로토콜 선택
- [ ] `[Common]` **TLS 1.3 전용 모드** — TLS 1.2 이하 비활성화 옵션
- [ ] `[Common]` **Session Resumption** — TLS 1.3 0-RTT / Session Tickets
- [ ] `[Common]` **OCSP Stapling** — 인증서 유효성 캐시 (클라이언트 RTT 제거)

### Linux 전용

- [ ] `[Linux]`  **`KTLS` (Kernel TLS)** — `setsockopt(TCP_ULP, "tls")` zero-copy 암호화
  - [ ] Kernel 5.2+ `TLS_TX` / `TLS_RX` 모드
  - [ ] io_uring + kTLS 연동 (암호화된 sendfile)

---

## 🟠 Phase 11: HTTP/2 지원

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
- [ ] `[Common]` **ALPN 통합** — Phase 10 TLS ALPN과 연동하여 h2 자동 선택

---

## 🟠 Phase 12: HTTP/3 / QUIC 지원

> 목표: 차세대 프로토콜 — 0-RTT, 연결 마이그레이션, 패킷 손실 복원

- [ ] `[Common]` **QUIC 라이브러리 통합** — quiche (Cloudflare) 또는 msquic (Microsoft) 선택
- [ ] `[Common]` **UDP 소켓 비동기 처리** — io_uring `IORING_OP_RECVMSG` / kqueue `EVFILT_READ`
- [ ] `[Common]` **QUIC 핸드셰이크** — TLS 1.3 over QUIC, 0-RTT 재연결
- [ ] `[Common]` **스트림 다중화** — QUIC stream ID 기반 병렬 요청
- [ ] `[Common]` **연결 마이그레이션** — CID 기반 IP 변경 추적 (모바일 네트워크 전환)
- [ ] `[Common]` **패킷 손실 복원** — QUIC 자체 재전송 (TCP retransmit 대비 빠름)
- [ ] `[Common]` **`Alt-Svc` 헤더** — HTTP/1.1·2 응답에 `h3=":443"` 광고
- [ ] `[Common]` **HTTP/3 시맨틱** — QPACK 헤더 압축, pseudo-headers

---

## 🟠 Phase 13: WebSocket 지원

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
- [ ] `[Common]` **Broadcast 지원** — lock-free 연결 레지스트리 + 멀티캐스트 전송
- [ ] `[Common]` **WS over TLS** (wss://) — Phase 10 TLS 통합
- [ ] `[Common]` **Subprotocol 협상** — `Sec-WebSocket-Protocol` 헤더
- [ ] `[Common]` **per-message deflate** — WS 압축 확장 (RFC 7692)

---

## 🟠 Phase 14: Streaming & SSE (Server-Sent Events)

- [ ] `[Common]` **Response Streaming API** — `res.write(chunk)` + `res.end()`
- [ ] `[Common]` **Server-Sent Events (SSE)**
  - [ ] `text/event-stream` 콘텐츠 타입 응답
  - [ ] `SseStream` 헬퍼: `send_event(name, data, id)` API
  - [ ] heartbeat (`comment:` ping) 자동 전송
- [ ] `[Common]` **파일 스트리밍** — 대용량 파일 청크 전송 (메모리에 올리지 않음)
- [ ] `[Common]` **백프레셔 (Backpressure)** — 소켓 쓰기 버퍼 포화 시 일시정지/재개 (`co_await` 기반)
- [ ] `[Common]` **Multipart 응답** — `multipart/x-mixed-replace` (MJPEG 스트림 등)

---

## 🟠 Phase 15: gRPC 지원

> 목표: HTTP/2 기반 gRPC — Protocol Buffers + 스트리밍

- [ ] `[Common]` **Protobuf 통합** — `protobuf` 라이브러리 FetchContent / vcpkg
- [ ] `[Common]` **gRPC 프레임 파싱** — 5바이트 length-prefix 프레임
- [ ] `[Common]` **gRPC 서비스 등록 API**
  - [ ] `app.grpc<Service>(handler)` 라우팅
  - [ ] Unary RPC
  - [ ] Server Streaming RPC
  - [ ] Client Streaming RPC
  - [ ] Bidirectional Streaming RPC
- [ ] `[Common]` **`grpc-status` / `grpc-message` Trailer** — HTTP/2 Trailer 처리
- [ ] `[Common]` **gRPC-Web** — 브라우저 호환 (HTTP/1.1 + base64 인코딩)
- [ ] `[Common]` **Deadline / Timeout** — `grpc-timeout` 헤더 파싱 및 `AsyncSleep` 연동
- [ ] `[Common]` **인터셉터 체인** — 미들웨어 패턴의 gRPC 인터셉터

---

## 🟡 Phase 16: 네트워크 심화 기능

### 공통

- [ ] `[Common]` **IPv6 완전 지원** — `::1` 바인딩, dual-stack `IPV6_V6ONLY` 설정
- [ ] **Unix Domain Socket** — `app.listen("/tmp/draco.sock")` 지원
  - [ ] `[Linux]`  `AF_UNIX` SOCK_STREAM
  - [ ] `[macOS]`  `AF_UNIX` SOCK_STREAM
- [ ] `[Common]` **리버스 프록시 지원**
  - [ ] `X-Forwarded-For` / `X-Real-IP` 헤더 파싱
  - [ ] Trusted proxy 화이트리스트 설정
- [ ] `[Common]` **Virtual Hosting** — Host 헤더 기반 라우터 분기
- [ ] `[Common]` **비동기 HTTP 클라이언트** — 백엔드 API 호출용 내장 클라이언트
  - [ ] `co_await client.get(url, headers)` / `co_await client.post(...)`
  - [ ] Keep-alive pool (per-domain connection reuse)
  - [ ] Connection timeout / response timeout
- [ ] `[Common]` **DNS 조회 (비동기)** — getaddrinfo 블로킹 해결 (thread pool offload)

### Linux 전용

- [ ] `[Linux]`  **`TCP_FASTOPEN`** — 3-way handshake 없이 첫 패킷에 데이터 전송
- [ ] `[Linux]`  **`SO_INCOMING_CPU`** — RSS 기반 CPU 친화성 소켓 배분
- [ ] `[Linux]`  **NUMA 인식 스레드 배치** — `numa_alloc_onnode` / `set_mempolicy`
- [ ] `[Linux]`  **NIC IRQ Affinity 가이드** — `/proc/irq/*/smp_affinity` 설정 스크립트

### macOS 전용

- [ ] `[macOS]`  **`TCP_FASTOPEN`** — macOS 12+ 지원
- [ ] `[macOS]`  **`SO_NOSIGPIPE`** — SIGPIPE 억제

---

## 🟡 Phase 17: Observability & 메트릭

> 목표: 성능 페널티 없는 프로덕션 가시성 — lock-free atomic 카운터 기반

- [ ] `[Common]` **Lock-free 내장 메트릭**
  - [ ] `std::atomic<uint64_t>` per-reactor 카운터 (요청 수, 에러 수, 활성 연결 수)
  - [ ] 히스토그램 버킷 (레이턴시 분포 P50/P95/P99)
  - [ ] 메트릭 집계 — reactor별 카운터 → 원자적 합산
- [ ] `[Common]` **Prometheus 메트릭 엔드포인트** — `GET /metrics` text exposition 포맷
  - [ ] `draco_requests_total{method, status}` counter
  - [ ] `draco_request_duration_seconds` histogram
  - [ ] `draco_connections_active` gauge
  - [ ] `draco_memory_arena_bytes` gauge
- [ ] `[Common]` **OpenTelemetry 트레이싱**
  - [ ] `traceparent` / `tracestate` W3C 헤더 파싱
  - [ ] span 컨텍스트 미들웨어로 전파
  - [ ] OTLP HTTP/gRPC exporter (Phase 15 gRPC 활용)
- [ ] `[Common]` **구조화 로깅 (Structured Logging)**
  - [ ] JSON 라인 포맷 (`{"ts":..,"level":..,"msg":..,"req_id":..}`)
  - [ ] **비동기 ring-buffer 로거** — hot path에서 메모리 복사만, I/O는 background thread
  - [ ] log level runtime 변경 (`SIGUSR2` 트리거)
- [ ] `[Common]` **요청 추적 (Distributed Tracing)** — `X-Request-ID` + `traceparent` 연동

---

## 🟡 Phase 18: URL / Request 유틸리티

- [ ] `[Common]` **URL 파싱** — path, query string, fragment 완전 분리
- [ ] `[Common]` **Query String 파싱** — `?foo=bar&baz=1` → `req.query("foo")` API
- [ ] `[Common]` **URL 인코딩/디코딩** — percent-encoding 유틸리티
- [ ] `[Common]` **Path Normalization** — `..` / `.` 제거, 이중 슬래시 처리
- [ ] `[Common]` **MIME Type 레지스트리** — 확장자 → Content-Type 자동 매핑 (컴파일 타임 해시맵)
- [ ] `[Common]` **Content Negotiation** — `Accept:` 헤더 q-factor 파싱 및 협상
- [ ] `[Common]` **`req.ip()`** — 실제 클라이언트 IP 추출 (프록시 헤더 포함)
- [ ] `[Common]` **Multipart Form Data 파서** — 바이너리 파일 업로드 포함

---

## 🟡 Phase 19: 성능 & 벤치마킹

> 목표: TechEmpower 프레임워크 벤치마크 Top 10 진입

### 공통

- [ ] `[Common]` **내장 벤치마크 스위트**
  - [ ] `wrk` / `wrk2` / `k6` 스크립트 제공 (`benchmarks/` 디렉토리)
  - [ ] Latency P50 / P95 / P99 / P99.9 측정
  - [ ] Throughput (RPS) 측정 자동화
  - [ ] **TechEmpower Fortune / JSON / Plaintext** 호환 엔드포인트
- [ ] `[Common]` **C10M 스트레스 테스트**
  - [ ] 10M 동시 연결 목표 시나리오
  - [ ] 연결당 메모리 사용량 측정 (목표: < 1KB/conn)
- [ ] `[Common]` **Flame Graph 통합** — 프로파일링 가이드 (`perf` / Instruments)
- [ ] `[Common]` **Zero-copy Serialization 최적화**
  - [ ] 응답 헤더 iovec scatter-gather 직렬화 (writev)
  - [ ] Beast JSON write buffer chain 최적화
- [ ] `[Common]` **Lock-free 라우팅 테이블** — 원자적 업데이트, RCU 패턴
- [ ] `[Common]` **pre-serialized 정적 응답** — `200 OK` / `404 Not Found` 바이트 배열 컴파일 타임 생성

### Linux 전용

- [ ] `[Linux]`  **`perf stat`** 통합 테스트 — cache miss / branch mispred 자동 리포트
- [ ] `[Linux]`  **io_uring SQ Polling** (`IORING_SETUP_SQPOLL`) — steady-state syscall 0
- [ ] `[Linux]`  **`taskset` + `chrt -f`** 실시간 우선순위 가이드

### macOS 전용

- [ ] `[macOS]`  **Instruments / DTrace 프로파일링 가이드**
- [ ] `[macOS]`  **`kqueue` kevent 배치 처리** — `nevents` 최대화 튜닝

---

## 🟡 Phase 20: 테스트 & CI/CD 강화

- [ ] `[Common]` **통합 테스트 확장**
  - [ ] Keep-alive 커넥션 재사용 테스트
  - [ ] Chunked 인코딩 수신/송신 테스트
  - [ ] 미들웨어 체인 순서 테스트
  - [ ] 에러 핸들러 테스트
  - [ ] Concurrent 요청 경쟁 조건 테스트
  - [ ] Request smuggling 방어 테스트
  - [ ] 대용량 파일 스트리밍 테스트
- [ ] `[Common]` **Fuzzing** — libFuzzer 기반 HTTP 파서 퍼징 (`corpus/` 제공)
- [ ] `[Common]` **Coverage Report** — lcov / gcovr + HTML 리포트 (목표: 90%+)
- [ ] `[Common]` **Sanitizer CI 통합**
  - [ ] AddressSanitizer (ASan)
  - [ ] UndefinedBehaviorSanitizer (UBSan)
  - [ ] ThreadSanitizer (TSan) — race condition 검출
  - [ ] MemorySanitizer (MSan) — 미초기화 읽기 검출
- [ ] `[Linux]`  **Valgrind / Helgrind** race condition 검사
- [ ] `[macOS]`  **Leaks / MallocStackLogging** 메모리 누수 검사
- [ ] `[Common]` **GitHub Actions 매트릭스 확장**
  - [ ] `ubuntu-22.04` + `ubuntu-24.04` (GCC 13, Clang 17, Clang 18)
  - [ ] `macos-13` (x86_64) + `macos-14` (Apple Silicon arm64)
  - [ ] Nightly 벤치마크 자동화 (PR 대비 성능 회귀 감지, 5% 이상 저하 시 fail)
  - [ ] Sanitizer 빌드 별도 매트릭스

---

## 🟢 Phase 21: 문서화 & DX (Developer Experience)

- [ ] `[Common]` **API 레퍼런스 문서** — Doxygen + GitHub Pages 자동 배포
- [ ] `[Common]` **Getting Started 가이드** — 5분 안에 Hello World 실행
- [ ] `[Common]` **예제 확장**
  - [ ] REST API 서버 (CRUD + JWT 인증)
  - [ ] WebSocket 채팅 서버
  - [ ] SSE 실시간 피드
  - [ ] 정적 파일 서버 (SPA 호스팅)
  - [ ] TLS HTTPS 서버
  - [ ] HTTP/2 서버
  - [ ] HTTP/3 / QUIC 서버
  - [ ] gRPC 서버
  - [ ] Prometheus + Grafana 연동 예제
- [ ] `[Common]` **Migration Guide** — v0.x → v1.0 Breaking Change 문서
- [ ] `[Common]` **성능 튜닝 가이드**
  - [ ] `[Linux]`  io_uring 커널 파라미터 (`ulimit -n`, `nr_open`, `memlock`, `SQPOLL` 권한)
  - [ ] `[Linux]`  OS 튜닝 스크립트 (`scripts/tune_linux.sh`) — sysctl, IRQ affinity, CPU governor
  - [ ] `[macOS]`  kqueue / `kern.maxfilesperproc` 설정 가이드
- [ ] `[Common]` **아키텍처 다이어그램** — Reactor / Dispatcher / Handler 흐름도 (Mermaid)
- [ ] `[Common]` **보안 가이드** — TLS 설정, 보안 헤더, Rate Limiting 베스트 프랙티스

---

## 📌 Phase 우선순위 요약

| Phase | 핵심 내용 | 플랫폼 | 중요도 |
|-------|----------|--------|--------|
| **5**  | io_uring 완성 + Zero-Copy I/O (MSG_ZEROCOPY, SQPOLL, AF_XDP) | Linux / macOS | 🔴 Critical |
| **6**  | Zero-Cost C++ (CRTP, SIMD 파서, LTO/PGO, Cache-line 정렬) | Common | 🔴 Critical |
| **7**  | HTTP/1.1 완전 준수 (Keep-Alive, Chunked, Timeout, TCP 튜닝) | Common | 🔴 Critical |
| **8**  | 핵심 미들웨어 (CORS, Body Parser, Rate Limit, 비동기 로깅) | Common | 🔴 Critical |
| **9**  | 보안 하드닝 (Smuggling 방지, HSTS, CSP, CSRF, 상수시간 비교) | Common | 🔴 Critical |
| **10** | TLS/SSL (HTTPS, SNI, ALPN, TLS 1.3, KTLS) | Common + Linux | 🔴 Critical |
| **11** | HTTP/2 (nghttp2, HPACK, Server Push, Flow Control) | Common | 🟠 High |
| **12** | HTTP/3 / QUIC (0-RTT, 연결 마이그레이션, Alt-Svc) | Common | 🟠 High |
| **13** | WebSocket RFC 6455 (per-message deflate, Broadcast) | Common | 🟠 High |
| **14** | Streaming / SSE / Backpressure | Common | 🟠 High |
| **15** | gRPC (Protobuf, 4가지 스트리밍 모드, gRPC-Web) | Common | 🟠 High |
| **16** | 네트워크 심화 (IPv6, UDS, 비동기 HTTP 클라이언트, NUMA) | Common + Linux + macOS | 🟡 Medium |
| **17** | Observability (Prometheus, OpenTelemetry, 비동기 로거) | Common | 🟡 Medium |
| **18** | URL / Request 유틸리티 | Common | 🟡 Medium |
| **19** | 성능 & 벤치마킹 (C10M, TechEmpower, PGO, BOLT) | Common + Linux + macOS | 🟡 Medium |
| **20** | 테스트 & CI/CD (Fuzzing, MSan, Nightly Regression) | Common + Linux + macOS | 🟡 Medium |
| **21** | 문서화 & DX (API Ref, OS 튜닝 스크립트, 보안 가이드) | Common | 🟢 Low |

---

## 📝 Contribution & Issues

- 버그는 재현 가능한 C++ 예제와 함께 GitHub Issue 로 제출
- 성능 프로파일링: `[Linux]` `perf record -g` / `[macOS]` Instruments Time Profiler
- PR 제출 전 `clang-tidy`, `AddressSanitizer`, `ThreadSanitizer` 통과 필수
- 성능 PR은 `wrk` 벤치마크 before/after 수치 첨부

---

*Created by The LKB Innovations.*
