# qbuem-stack IO Layer Architecture

> **목표**: Zero-latency · Zero-allocation · Zero-dependency
> C++ 서버 플랫폼의 전체 IO 스택을 한 번에 정의한다.

---

## 1. 설계 원칙

| 원칙 | 구체적 규칙 |
|------|-------------|
| **Zero Allocation (hot path)** | accept→처리→응답 경로에서 `new`/`malloc` 금지. Arena + FixedPool + stack 전용 |
| **Zero Dependency** | 헤더 공개 API에서 외부 라이브러리 노출 금지. 구현부(`.cpp`) 내부 한정 허용 |
| **Zero Copy** | recv → parse → send 경로에서 memcpy 금지. IOVec scatter-gather, sendfile, MSG_ZEROCOPY |
| **Shared-Nothing** | Reactor 1개 = Thread 1개. 스레드 간 공유 자료구조 없음 |
| **C++20 only** | Coroutines, Concepts, span, ranges. POSIX 시스템콜 직접 사용 |
| **Platform headers only** | `<sys/socket.h>`, `<sys/uio.h>`, `<linux/io_uring.h>` — 커널/libc 헤더만 |

---

## 2. 레이어 구조

```
┌──────────────────────────────────────────────────────────────────────┐
│  Layer 8: Protocol Handlers (v1.0)                                   │
│  Http1Handler  Http2Handler  WebSocketHandler  GrpcHandler           │
├──────────────────────────────────────────────────────────────────────┤
│  Layer 7: Codec / Framing (v0.9)                                     │
│  IFrameCodec<F>  LengthPrefixedCodec  LineCodec  Http1Codec          │
├──────────────────────────────────────────────────────────────────────┤
│  Layer 6: Connection Lifecycle (v0.8)                                │
│  AcceptLoop  IConnectionHandler<Frame>  ConnectionPool<T>            │
├────────────────────────────┬─────────────────────────────────────────┤
│  Layer 5: Transport (v0.7) │  Layer 5b: Zero-Copy IO (v0.7)         │
│  ITransport (exists)       │  Sendfile  Splice  MsgZerocopy          │
│  PlainTransport (NEW)      │  AsyncFile (io_uring / pread fallback)  │
├────────────────────────────┴─────────────────────────────────────────┤
│  Layer 4: Buffer / IO Slice (v0.7)                                   │
│  IOSlice  IOVec<N>  ReadBuf<N>  WriteBuf                            │
├──────────────────────────────────────────────────────────────────────┤
│  Layer 3: Network Primitives (v0.7)                                  │
│  SocketAddr  TcpListener  TcpStream  UdpSocket  UnixSocket           │
├──────────────────────────────────────────────────────────────────────┤
│  Layer 2: Event Loop / Reactor (exists)                              │
│  Reactor  EpollReactor  KqueueReactor  IOUringReactor  Dispatcher    │
│  TimerWheel (NEW, v0.7 — O(1) insert/cancel/expire)                 │
├──────────────────────────────────────────────────────────────────────┤
│  Layer 1: Memory (exists)                                            │
│  Arena  FixedPoolResource  BufferPool<N> (NEW)                      │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 3. 파일 구조

```
include/qbuem/
  core/               ← 기존 (Reactor, Arena, Task, Transport...)
    net/              ← NEW: 네트워크 소켓 프리미티브
      socket_addr.hpp
      tcp_listener.hpp
      tcp_stream.hpp
      udp_socket.hpp
      unix_socket.hpp
    io/               ← NEW: 버퍼 / 슬라이스 / Zero-copy
      io_slice.hpp
      read_buf.hpp
      write_buf.hpp
      async_file.hpp
      zero_copy.hpp   (Sendfile, Splice, MsgZerocopy)
    timer_wheel.hpp   ← NEW: O(1) 계층적 타이머
  codec/              ← NEW: 프로토콜 프레임 코덱
    frame_codec.hpp
    length_prefix_codec.hpp
    line_codec.hpp
    http1_codec.hpp
  net/                ← NEW: 연결 수명 관리
    accept_loop.hpp
    connection_handler.hpp
    connection_pool.hpp
  protocol/           ← v1.0: 프로토콜 핸들러
    http1_handler.hpp
    http2_handler.hpp
    websocket_handler.hpp
    grpc_handler.hpp
```

---

## 4. Layer 1 — Memory 확장

### 4.1 `BufferPool<N>` (신규)

```cpp
// FixedPoolResource 위의 얇은 래퍼 — io_uring Buffer Ring과 연동
template<size_t BufSize, size_t Count>
class BufferPool {
    FixedPoolResource<BufSize> pool_{Count};
    // io_uring buf_ring 연동용 iovec 테이블
    iovec iovecs_[Count];
public:
    // O(1) 할당. nullptr = 고갈 (backpressure 신호)
    std::byte* acquire() noexcept;
    // O(1) 반환.
    void release(std::byte* buf) noexcept;
    // io_uring fixed buffer 등록용
    std::span<const iovec> iovecs() const noexcept;
    static constexpr size_t buf_size() noexcept { return BufSize; }
    static constexpr size_t count()    noexcept { return Count; }
};

// 전형적 사용:
// per-reactor 정적 풀 (컴파일타임 크기)
using RxPool = BufferPool<4096, 1024>;   // 4MB per reactor
using TxPool = BufferPool<65536, 64>;    // 4MB per reactor
```

---

## 5. Layer 2 — TimerWheel (신규)

현재 `register_timer()` 구현은 `std::unordered_map` + 최소힙(epoll/timerfd)을 사용.
연결 수 증가 시 O(log N) 오버헤드 + 힙 할당 발생.

### 5.1 계층적 타이밍 휠 (Hierarchical Timing Wheel)

```
4 레벨 × 256 슬롯 = 전체 범위: ~4.6시간 (ms 해상도)

Level 0: 슬롯 0-255  →  1ms 단위     (0 ~ 255ms)
Level 1: 슬롯 0-255  →  256ms 단위   (0 ~ 65s)
Level 2: 슬롯 0-255  →  65.5s 단위   (0 ~ 4.6h)
Level 3: 슬롯 0-255  →  4.7h 단위    (overflow)
```

```cpp
class TimerWheel {
public:
    explicit TimerWheel() noexcept;

    // O(1): 타이머 등록. 콜백은 Reactor 스레드에서 호출됨.
    uint64_t schedule(uint64_t delay_ms, std::move_only_function<void()> cb) noexcept;

    // O(1): 타이머 취소.
    bool cancel(uint64_t timer_id) noexcept;

    // O(expired): 시계 전진. poll() 루프에서 호출.
    size_t tick(uint64_t now_ms) noexcept;

    // 다음 만료까지 남은 시간 (ms). poll timeout 계산에 사용.
    uint64_t next_expiry_ms(uint64_t now_ms) const noexcept;

private:
    struct Slot {
        struct Entry {
            uint64_t                       expire_ms;
            uint64_t                       id;
            std::move_only_function<void()> cb;
            Entry* next = nullptr;  // intrusive linked list (FixedPool 사용)
        };
        Entry* head = nullptr;
    };
    Slot levels_[4][256];
    FixedPoolResource<sizeof(Slot::Entry)> pool_{65536};
    uint64_t now_ms_ = 0;
    uint64_t next_id_ = 1;
};
```

**핵심**: `Entry` 할당을 `FixedPoolResource`로 처리 → timer schedule/cancel 모두 zero-heap.

---

## 6. Layer 3 — Network Primitives

### 6.1 `SocketAddr` — value type, zero-alloc

```cpp
class SocketAddr {
    union {
        sockaddr_in  v4;
        sockaddr_in6 v6;
        sockaddr_un  un;
    };
    socklen_t len_;
    enum class Family : uint8_t { IPv4, IPv6, Unix } family_;

public:
    // 파싱: 실패 시 빈 SocketAddr 반환 (port==0)
    static SocketAddr ipv4(std::string_view host, uint16_t port) noexcept;
    static SocketAddr ipv6(std::string_view host, uint16_t port) noexcept;
    static SocketAddr unix_path(std::string_view path) noexcept;

    // 시스템콜 직접 전달용
    const sockaddr* sa()    const noexcept;
    socklen_t       sa_len()const noexcept;

    uint16_t port()         const noexcept;
    bool     is_ipv4()      const noexcept;
    bool     is_ipv6()      const noexcept;
    bool     is_unix()      const noexcept;

    // 할당 없는 문자열 변환: "1.2.3.4:8080" 형태로 buf에 기록
    size_t   to_chars(std::span<char> buf) const noexcept;
    bool     operator==(const SocketAddr&) const noexcept;
};
```

### 6.2 `TcpListener`

```cpp
class TcpListener {
public:
    // SO_REUSEPORT: 각 reactor가 독립적인 소켓으로 bind
    // → kernel이 accept를 worker 간에 load-balance
    static Result<TcpListener> bind(SocketAddr addr,
                                    int backlog    = 512,
                                    bool reuse_port = true) noexcept;

    // co_await 가능: (fd, peer_addr) 또는 에러
    Task<Result<std::pair<int, SocketAddr>>> accept(Reactor& r);

    int  fd()   const noexcept;
    void close()      noexcept;

    // non-copyable, movable
    TcpListener(const TcpListener&) = delete;
    TcpListener(TcpListener&&)      noexcept;
};
```

**SO_REUSEPORT 전략**:
```
Dispatcher
  ├── Worker 0: Reactor0 ← TcpListener(8080, reuse_port=true) ← kernel
  ├── Worker 1: Reactor1 ← TcpListener(8080, reuse_port=true) ← kernel
  └── Worker 2: Reactor2 ← TcpListener(8080, reuse_port=true) ← kernel

accept() 경합 없음 — kernel이 CPU affinity 기반으로 분배
```

### 6.3 `TcpStream`

```cpp
class TcpStream {
public:
    static Result<TcpStream>  connect(SocketAddr addr) noexcept;
    static TcpStream          from_fd(int fd, SocketAddr peer) noexcept;

    // Vectored I/O — 단일 시스템콜로 scatter-gather
    Task<Result<size_t>> readv (std::span<iovec>       iov, Reactor& r);
    Task<Result<size_t>> writev(std::span<const iovec> iov, Reactor& r);

    // 편의 단일 버퍼 오버로드 (내부적으로 iovec[1] 사용, 스택 할당)
    Task<Result<size_t>> read (std::span<std::byte>       buf, Reactor& r);
    Task<Result<size_t>> write(std::span<const std::byte> buf, Reactor& r);

    // TCP 옵션
    Result<void> set_nodelay   (bool enable)                    noexcept;
    Result<void> set_keepalive (bool enable, int idle_sec = 60) noexcept;
    Result<void> set_recv_buf  (int bytes)                      noexcept;
    Result<void> set_send_buf  (int bytes)                      noexcept;

    void         shutdown(int how) noexcept;  // SHUT_RD / SHUT_WR / SHUT_RDWR
    void         close()           noexcept;

    int        fd()         const noexcept;
    SocketAddr peer_addr()  const noexcept;
    SocketAddr local_addr() const noexcept;
};
```

### 6.4 `UdpSocket`

```cpp
class UdpSocket {
public:
    static Result<UdpSocket> bind  (SocketAddr addr) noexcept;
    static Result<UdpSocket> create(bool ipv6 = false) noexcept;

    // 단일 데이터그램
    Task<Result<std::pair<size_t, SocketAddr>>>
        recvfrom(std::span<std::byte> buf, Reactor& r);

    Task<Result<size_t>>
        sendto(std::span<const std::byte> buf, SocketAddr dest, Reactor& r);

    // Batch recv (io_uring IORING_OP_RECVMSG_MULTI — Linux 6.0+)
    // batch_size개 메시지를 단일 SQE로 처리
    Task<Result<size_t>>
        recvmsg_batch(std::span<mmsghdr> msgs, Reactor& r);

    int  fd()   const noexcept;
    void close()      noexcept;
};
```

---

## 7. Layer 4 — Buffer / IO Slice

### 7.1 `IOSlice` / `IOVec<N>` — zero-copy scatter-gather

```cpp
// 단순 읽기 전용 슬라이스 (fat pointer)
struct IOSlice {
    const std::byte* data;
    size_t           size;

    std::span<const std::byte> as_span() const noexcept;
    bool empty() const noexcept { return size == 0; }
};

// 스택 할당 scatter-gather 배열
// writev / sendmsg / io_uring IORING_OP_WRITEV 직접 전달
template<size_t MaxVec = 8>
struct IOVec {
    iovec  iov[MaxVec];
    size_t count = 0;

    void push(const void* data, size_t len) noexcept;
    void push(IOSlice s)                   noexcept;
    void push(std::span<const std::byte> s) noexcept;

    size_t total_bytes()    const noexcept;
    bool   full()           const noexcept { return count == MaxVec; }

    // 시스템콜 직접 전달
    const iovec* data() const noexcept { return iov; }
    int          size() const noexcept { return static_cast<int>(count); }
};
```

### 7.2 `ReadBuf<N>` — 컴파일타임 고정 링버퍼, zero-alloc

```cpp
// 연결당 수신 버퍼 — 스택 또는 Arena에 저장
// N: 전형적으로 16KB (HTTP/1.1) ~ 256KB (HTTP/2)
template<size_t N>
class ReadBuf {
    std::byte buf_[N];
    size_t    read_pos_  = 0;   // 파서가 소비한 위치
    size_t    write_pos_ = 0;   // recv()가 채운 위치
public:
    // recv() syscall에 넘길 쓰기 가능 영역
    std::span<std::byte>       write_head()                noexcept;
    void                       commit(size_t n)            noexcept;

    // 파서에게 넘길 읽기 가능 영역
    std::span<const std::byte> read_head()           const noexcept;
    void                       consume(size_t n)           noexcept;

    // 링버퍼 공간 재활용 (read_pos == write_pos이면 리셋)
    void                       compact()                   noexcept;

    size_t readable()          const noexcept;
    size_t writable()          const noexcept;
    bool   empty()             const noexcept;
    bool   full()              const noexcept;

    static constexpr size_t capacity() noexcept { return N; }
};
```

### 7.3 `WriteBuf` — 코르크 버퍼 (cork/flush)

```cpp
// 작은 쓰기를 묶어 writev() 한 번에 전송
// 내부 버퍼: Arena 또는 BufferPool에서 슬라이스 참조
class WriteBuf {
public:
    explicit WriteBuf(Arena& arena) noexcept;

    // Arena에서 슬라이스 할당 후 반환된 span에 직접 기록
    std::span<std::byte> prepare(size_t n);
    void                 confirm(size_t n) noexcept;

    // 이미 할당된 버퍼 참조 추가 (zero-copy path)
    void append_ref(IOSlice slice) noexcept;

    // writev() 전달용 IOVec 구성 (스택, 최대 64 iovec)
    IOVec<64> as_iovec() const noexcept;

    void clear() noexcept;
    bool empty() const noexcept;
    size_t pending_bytes() const noexcept;
};
```

### 7.4 `AsyncFile` — 비동기 파일 IO

```cpp
// io_uring IORING_OP_READ/WRITE_FIXED 우선, 없으면 pread/pwrite 폴백
class AsyncFile {
public:
    static Task<Result<AsyncFile>> open(std::string_view path,
                                        int flags,  // O_RDONLY | O_DIRECT ...
                                        mode_t mode = 0644);

    // 지정 오프셋에서 읽기 (고정 버퍼 사용 시 zero-copy)
    Task<Result<size_t>> read_at (std::span<std::byte>       buf,
                                  int64_t offset, Reactor& r);
    Task<Result<size_t>> write_at(std::span<const std::byte> buf,
                                  int64_t offset, Reactor& r);

    // 순차 읽기 (오프셋 자동 증가)
    Task<Result<size_t>> read (std::span<std::byte>       buf, Reactor& r);
    Task<Result<size_t>> write(std::span<const std::byte> buf, Reactor& r);

    Task<Result<void>>   close();
    int64_t              size() const noexcept;
    int                  fd()   const noexcept;
};
```

### 7.5 `ZeroCopy` — sendfile / splice / MSG_ZEROCOPY

```cpp
namespace zero_copy {

// sendfile(2): 파일 → 소켓 직접 전송 (kernel space only, no user copy)
// 정적 파일 서빙 최적화
Task<Result<size_t>>
sendfile(int out_sock_fd, int in_file_fd,
         int64_t offset, size_t count,
         Reactor& r);

// splice(2): pipe를 통한 fd → fd 전송 (generic zero-copy)
Task<Result<size_t>>
splice(int in_fd, int64_t* in_off,
       int out_fd, int64_t* out_off,
       size_t count, unsigned flags,
       Reactor& r);

// MSG_ZEROCOPY: send() 완료를 비동기 통지 (Linux 4.14+)
// 대용량 동적 응답에서 사용
struct ZeroCopyCtx {
    uint32_t seq_lo, seq_hi;  // 완료 추적용 시퀀스 범위
};
Task<Result<ZeroCopyCtx>>
send_zerocopy(int fd,
              std::span<const std::byte> buf,
              Reactor& r);

// MSG_ZEROCOPY 완료 대기 (errqueue polling)
Task<Result<void>> wait_zerocopy(int fd, ZeroCopyCtx ctx, Reactor& r);

} // namespace zero_copy
```

---

## 8. Layer 5 — Transport 확장

### 8.1 `PlainTransport` — 구체 TCP 구현체 (신규)

```cpp
// ITransport의 plain TCP 구현
// TLS 없음 — 서비스에서 OpenSSL/mbedTLS 구현체 주입
class PlainTransport final : public ITransport {
public:
    explicit PlainTransport(TcpStream stream) noexcept;

    Task<Result<size_t>> read (std::span<std::byte>       buf) override;
    Task<Result<size_t>> write(std::span<const std::byte> buf) override;
    Task<Result<void>>   handshake()                           override; // no-op
    Task<Result<void>>   close()                               override;

    // TcpStream 직접 접근 (zero-copy path에서 사용)
    TcpStream& stream() noexcept;

private:
    TcpStream stream_;
};
```

---

## 9. Layer 6 — Codec / Framing

### 9.1 `IFrameCodec<Frame>` — zero-alloc 코덱 인터페이스

```cpp
// Frame: 코덱이 생산/소비하는 타입.
//        보통 스택 할당 구조체 (예: Http1Request, WsFrame).
template<typename Frame>
class IFrameCodec {
public:
    virtual ~IFrameCodec() = default;

    // 디코드: buf에서 프레임 하나를 out에 씀.
    // 반환값: 소비한 바이트 수. 0 = 데이터 부족 (more_data 필요).
    // 에러: Result의 에러 코드.
    virtual Result<size_t> decode(std::span<const std::byte> buf,
                                  Frame& out) noexcept = 0;

    // 인코드: frame을 iov에 슬라이스 목록으로 채움.
    // iov의 slice들이 가리키는 메모리는 caller가 관리 (Arena 등).
    virtual Result<void>   encode(const Frame& frame,
                                  IOVec<16>& iov,
                                  Arena& arena) noexcept = 0;
};
```

### 9.2 `LengthPrefixedCodec<Header>` — 길이 헤더 프레임

```cpp
// Header: 고정 크기 헤더 타입. length() 메서드 필요.
// 예: struct MyHeader { uint32_t magic; uint32_t length() const; };
template<typename Header>
class LengthPrefixedCodec final : public IFrameCodec<std::span<const std::byte>> {
public:
    explicit LengthPrefixedCodec(size_t max_frame_size = 64 * 1024) noexcept;

    Result<size_t> decode(std::span<const std::byte> buf,
                          std::span<const std::byte>& out) noexcept override;
    Result<void>   encode(const std::span<const std::byte>& frame,
                          IOVec<16>& iov, Arena& arena) noexcept override;
};
```

### 9.3 `LineCodec` — 줄바꿈 구분 프레임

```cpp
// RESP, Redis, SMTP, NNTP 등
class LineCodec final : public IFrameCodec<std::span<const std::byte>> {
public:
    explicit LineCodec(std::byte delim = std::byte{'\n'},
                       size_t max_line = 8192) noexcept;

    Result<size_t> decode(std::span<const std::byte> buf,
                          std::span<const std::byte>& out) noexcept override;
    Result<void>   encode(const std::span<const std::byte>& line,
                          IOVec<16>& iov, Arena& arena) noexcept override;
};
```

### 9.4 `Http1Codec` — HTTP/1.1 (기존 parser.hpp 래핑)

```cpp
// 기존 qbuem::http::Parser를 IFrameCodec으로 래핑
// Request/Response 모두 Arena 할당 (헤더 값 슬라이스 → Arena에 복사 없음)
class Http1Codec final : public IFrameCodec<http::Request> {
public:
    Result<size_t> decode(std::span<const std::byte> buf,
                          http::Request& out) noexcept override;
    Result<void>   encode(const http::Request& req,
                          IOVec<16>& iov, Arena& arena) noexcept override;
};
```

---

## 10. Layer 7 — Connection Lifecycle

### 10.1 `IConnectionHandler<Frame>` — 연결 핸들러 인터페이스

```cpp
template<typename Frame>
class IConnectionHandler {
public:
    virtual ~IConnectionHandler() = default;

    // 새 연결 수락 후 호출 (handshake 포함)
    virtual Task<Result<void>> on_connect(Connection& conn,
                                          SocketAddr peer) = 0;

    // 프레임 수신 시마다 호출
    virtual Task<Result<void>> on_frame(Connection& conn,
                                        Frame&& frame) = 0;

    // 연결 종료 시 호출 (정상/에러 모두)
    virtual void on_disconnect(Connection& conn,
                                std::error_code ec) noexcept = 0;
};
```

### 10.2 `AcceptLoop` — SO_REUSEPORT 코루틴 루프

```cpp
// 각 Worker가 독립적인 TcpListener를 가짐 → accept 경합 없음
// Handler: IConnectionHandler<Frame>을 구현한 타입 (또는 lambda)
template<typename Frame, typename HandlerFactory>
    requires std::invocable<HandlerFactory>
          && std::derived_from<std::invoke_result_t<HandlerFactory>,
                               IConnectionHandler<Frame>>
Task<void> accept_loop(
    SocketAddr         bind_addr,
    HandlerFactory     make_handler,
    IFrameCodec<Frame>& codec,
    Arena&             arena,
    Reactor&           reactor,
    std::stop_token    stop
);

// 내부 동작:
// 1. TcpListener::bind(addr, reuse_port=true)
// 2. loop: auto [fd, peer] = co_await listener.accept(reactor)
// 3. auto conn = Connection{fd, reactor}
// 4. dispatcher.spawn(handle_connection(conn, peer, codec, make_handler(), arena))
```

### 10.3 `ConnectionPool<T>` — 아웃바운드 커넥션 풀

```cpp
template<typename T>  // T: TcpStream 또는 ITransport 구현체
class ConnectionPool {
public:
    struct Config {
        SocketAddr   target;
        size_t       min_idle   = 2;
        size_t       max_size   = 64;
        int          idle_timeout_ms   = 30'000;
        int          connect_timeout_ms = 5'000;
        int          health_check_ms   = 10'000;
    };

    explicit ConnectionPool(Config cfg, Reactor& reactor);

    // O(1) hot path: idle 연결 꺼냄
    // 없으면 새 연결 생성 (최대 max_size까지)
    Task<Result<std::unique_ptr<T, ReturnToPool>>> acquire();

    // 연결 반환 (RAII deleter가 자동 호출)
    void release(T* conn, bool healthy) noexcept;

    size_t idle_count()  const noexcept;
    size_t total_count() const noexcept;
};
```

---

## 11. v1.0 Protocol Handlers

### 11.1 HTTP/1.1

```cpp
// Http1Handler: IConnectionHandler<http::Request> 구현
// - keep-alive 자동 처리
// - 100-continue 지원
// - chunked transfer encoding
// - Upgrade 헤더 처리 (→ WebSocket upgrade)
// Router를 주입받아 요청 라우팅

class Http1Handler final : public IConnectionHandler<http::Request> {
public:
    explicit Http1Handler(http::Router& router, Arena& arena);
    // ... IConnectionHandler<http::Request> 구현
};
```

### 11.2 HTTP/2 (v1.0)

```cpp
// 외부 HPACK 구현 없음 — header table은 Arena 기반 구현
// SETTINGS, WINDOW_UPDATE, PING, GOAWAY 지원
// Stream multiplexing: AsyncChannel<Http2Frame> per stream

struct Http2Frame {
    uint8_t  type;
    uint8_t  flags;
    uint32_t stream_id;
    std::span<const std::byte> payload;  // Arena에서 살아있는 동안 유효
};

class Http2Handler final : public IConnectionHandler<Http2Frame> {
public:
    explicit Http2Handler(http::Router& router, Arena& arena);
    // ALPN "h2" 협상 후 Http1Codec → Http2Codec 전환
};
```

### 11.3 WebSocket (v1.0)

```cpp
struct WsFrame {
    bool   fin;
    uint8_t opcode;   // TEXT/BINARY/PING/PONG/CLOSE
    bool   masked;
    std::span<const std::byte> payload;  // unmasked, Arena 소유
};

class WebSocketHandler final : public IConnectionHandler<WsFrame> {
public:
    // HTTP/1.1 Upgrade 요청 검증 후 101 Switching Protocols 응답
    // 이후 WsFrame 루프
};
```

### 11.4 gRPC (v1.0)

```cpp
// HTTP/2 위에서 동작. protobuf 직접 의존 없음.
// 서비스에서 serialize/deserialize 구현 제공
// MessageBus Bidi채널 → gRPC 백엔드 연결

template<typename Req, typename Res>
class GrpcHandler {
public:
    // Unary
    virtual Task<Result<Res>>  handle_unary(Req req, ActionEnv env) = 0;
    // Server streaming → Stream<Res>
    virtual Task<Result<Stream<Res>>> handle_server_stream(Req req, ActionEnv env) = 0;
    // Bidi → AsyncChannel
    virtual Task<Result<void>> handle_bidi(
        AsyncChannel<Req>& in, AsyncChannel<Res>& out, ActionEnv env) = 0;
};
```

---

## 12. Zero-Dependency 전략

```
의존성 레벨        헤더 노출  구현 노출  비고
─────────────────────────────────────────────────────────────────────
POSIX sockets      YES        YES        zero-dep, 항상 사용 가능
Linux io_uring     NO (pimpl) YES(.cpp)  선택적. 없으면 epoll 폴백
liburing           NO (pimpl) YES(.cpp)  선택적. CMake 옵션
OpenSSL/TLS        NO         NO         서비스에서 ITransport 구현
nghttp2            NO         NO         서비스에서 ICodec 구현
protobuf           NO         NO         서비스에서 직렬화 구현
JSON               NO         NO         qbuem-json 별도 레포
zlib/brotli/zstd   NO         NO         서비스에서 IBodyEncoder 구현
```

**CMake feature flags:**
```cmake
option(QBUEM_IOURING  "Enable io_uring reactor"    ON)   # Linux only
option(QBUEM_EPOLL    "Enable epoll reactor"        ON)   # Linux only
option(QBUEM_KQUEUE   "Enable kqueue reactor"       ON)   # macOS/BSD only
option(QBUEM_SENDFILE "Enable sendfile zero-copy"   ON)
option(QBUEM_ZEROCOPY "Enable MSG_ZEROCOPY support" ON)   # Linux 4.14+
```

---

## 13. 구현 의존성 & 로드맵

```
v0.7.0 — IO Primitives (이 문서의 Layer 3-5)
  ├── SocketAddr / TcpListener / TcpStream / UdpSocket
  ├── IOSlice / IOVec<N> / ReadBuf<N> / WriteBuf
  ├── AsyncFile
  ├── PlainTransport
  ├── zero_copy:: (sendfile/splice/MSG_ZEROCOPY)
  └── TimerWheel O(1)

v0.8.0 — Codec + Connection Lifecycle (Layer 6-7)
  ├── IFrameCodec<F> / LengthPrefixedCodec / LineCodec / Http1Codec
  ├── AcceptLoop (SO_REUSEPORT)
  ├── IConnectionHandler<Frame>
  └── ConnectionPool<T>

v0.9.0 — Pipeline 고도화 (기존 TODO 유지)
  ├── DynamicPipeline::hot_swap
  ├── PriorityChannel<T>
  ├── SpscChannel<T> (Lamport)
  └── Rx operators

v1.0.0 — Protocol Handlers
  ├── Http1Handler (기존 router 연결)
  ├── Http2Handler (HPACK, stream multiplexing)
  ├── WebSocketHandler (RFC 6455)
  └── GrpcHandler<Req,Res> (protobuf injection)
```

---

## 14. Hot Path 메모리 흐름 (전체 zero-alloc 증명)

```
[accept]
  TcpListener::accept()           → int fd (스택)
  SocketAddr peer                 → 스택 value type

[연결 초기화]
  Connection conn{fd, reactor}    → FixedPoolResource<sizeof(Connection)>
  Arena arena{conn.arena()}       → 연결당 Arena (Connection 내부)
  ReadBuf<16384> rbuf             → 스택 (코루틴 프레임 내부)
  WriteBuf wbuf{arena}            → Arena 참조만

[수신]
  rbuf.write_head()               → rbuf의 스택 버퍼 참조
  TcpStream::read(rbuf.write_head(), reactor)
    → register_event (epoll) or POLL_ADD (io_uring)
    → 이벤트 후 recv() 시스템콜
  rbuf.commit(n)                  → 포인터 증가

[파싱]
  Http1Codec::decode(rbuf.read_head(), request)
    → request 헤더 값: rbuf 내부 포인터 (zero-copy slice)
    → request 자체: 스택 (코루틴 프레임)
  rbuf.consume(n)                 → 포인터 증가

[처리]
  router.dispatch(request, env)   → Pipeline Action 체인
  Arena로 응답 헤더/바디 구성     → bump-pointer, O(1)

[송신]
  wbuf.as_iovec()                 → 스택 IOVec<64>
  TcpStream::writev(iov, reactor) → 단일 writev() 시스템콜

[연결 종료]
  Connection 소멸                 → FixedPoolResource::release()
  Arena::reset()                  → 포인터 리셋 (OS 반환 없음)
```

**new/malloc 호출 횟수: 0**

---

## 15. 성능 목표

| 지표 | 목표 | 측정 방법 |
|------|------|-----------|
| Latency P50 | < 50µs | wrk2 constant rate |
| Latency P99 | < 500µs | wrk2 constant rate |
| Throughput | > 500k RPS (HTTP/1.1, 8core) | wrk / h2load |
| Accept rate | > 1M conn/s | custom benchmark |
| Memory / conn | < 32KB | resident set / connection count |
| Alloc / request | 0 (hot path) | custom allocator instrumentation |
