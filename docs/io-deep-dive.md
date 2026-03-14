# qbuem-stack IO 기술 심층 분석

> **범위**: 현재 구현된 IO 기반 + 향후 적용 가능한 고급 기술 전체 정리
>
> 관련 문서:
> - IO 레이어 아키텍처: [docs/io-architecture.md](./io-architecture.md)
> - 라이브러리 분리 전략: [docs/library-strategy.md](./library-strategy.md)

---

## 1. 현재 구현 상태

| 기술 | 구현 위치 | 상태 |
|------|-----------|------|
| `epoll` (LT) | `EpollReactor` | ✓ 완료 |
| `kqueue` | `KqueueReactor` | ✓ 완료 |
| `io_uring` POLL_ADD | `IOUringReactor` | ✓ 완료 |
| `io_uring` SQPOLL | `IOUringReactor` | ✓ 완료 |
| `io_uring` Fixed Buffers | `IOUringReactor` | ✓ 완료 |
| `io_uring` Buffer Ring | `IOUringReactor` | ✓ 완료 |
| `Reactor::post()` 크로스스레드 wakeup | 전 Reactor | ✓ 완료 |
| `SO_REUSEPORT` per-reactor | Dispatcher | ✓ 완료 |
| `writev()` scatter-gather | HTTP 응답 | ✓ 완료 |
| `sendfile(2)` static files | StaticFiles MW | ✓ 완료 |
| `SO_BUSY_POLL`, `TCP_FASTOPEN` | Dispatcher | ✓ 완료 |
| Arena + FixedPoolResource | 전 레이어 | ✓ 완료 |

---

## 2. io_uring 심층

### 2.1 현재: POLL_ADD 기반 (준비 알림)

```
[현재 동작]
SQE(POLL_ADD, fd) → 커널 → fd 준비 → CQE(fd ready) → 유저: read()/write() 수행
                                                                ↑
                                                        이 단계가 여전히 syscall
```

현재 `IOUringReactor`는 io_uring을 **준비 알림기**로만 사용한다.
실제 I/O는 여전히 `read()`/`write()` syscall로 수행.

### 2.2 목표: 완전 io_uring (비동기 I/O 직접 제출)

```
[목표 동작]
SQE(RECV, fd, buf) → 커널이 직접 recv 후 buf에 쓰기
                   → CQE(bytes_received) → 유저: 버퍼만 처리
                                            ↑
                                    syscall 없음
```

**구현 방향:**

```cpp
// 현재: POLL_ADD → read() 패턴
reactor->register_event(fd, EventType::Read, [fd, buf](int) {
    ssize_t n = ::read(fd, buf.data(), buf.size());  // ← 여기서 syscall
});

// 목표: 직접 RECV SQE 제출
reactor->submit_recv(fd, buf, [](int bytes) {  // ← SQE 제출만, syscall 없음
    // bytes: 완료된 바이트 수
});
```

### 2.3 Multishot Operations (Linux 5.19+) ★★★

**가장 높은 성능 향상 가능 기술**

#### `IORING_OP_ACCEPT_MULTISHOT`

```
기존: accept() CQE → 재제출(SQE) → accept() CQE → 재제출 ...
     SQE 제출이 연결마다 필요

Multishot: SQE 1회 제출 → 연결마다 CQE 자동 생성 (SQE 재제출 없음)
```

```cpp
// 의사 코드
io_uring_prep_multishot_accept(sqe, listen_fd, &addr, &addrlen, 0);
// 이후 새 연결마다 CQE 자동 생성, SQE는 자동 유지
// CQE.res = 새 client_fd
```

**효과**: high-concurrency 서버에서 accept 오버헤드 제거.

#### `IORING_OP_RECV_MULTISHOT` (Buffer Ring 연동)

```
기존:  recv() CQE → 처리 → 재제출 → recv() CQE ...
Multishot: SQE 1회 → 패킷마다 CQE 자동 생성
           Buffer Ring에서 커널이 버퍼 자동 선택
```

```cpp
// Buffer Ring 등록
io_uring_register_buf_ring(ring, &buf_ring_reg, 0);

// Multishot recv 제출 (1회만)
io_uring_prep_recv_multishot(sqe, client_fd, NULL, 0, 0);
sqe->buf_group = bgid;  // Buffer Ring 그룹 ID
sqe->flags |= IOSQE_BUFFER_SELECT;

// 각 CQE에서
uint16_t buf_id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
void *data = buf_ring_get_ptr(bgid, buf_id);
size_t len = cqe->res;
// ... 처리 ...
io_uring_buf_ring_add(...);  // 버퍼 반환
```

**효과**: 연결당 recv SQE 재제출 없음. 커널이 버퍼 자동 선택으로 zero-copy 경로.

### 2.4 Fixed Files (파일 디스크립터 등록) ★★

```
기존: 모든 SQE에서 fd → 커널이 파일 테이블에서 fd 조회 (RCU lock)
Fixed: io_uring_register_files() → 이후 SQE에서 인덱스 사용, 테이블 조회 없음
```

```cpp
// 연결 수락 후 fd 등록
int fds[MAX_CONNS] = {-1};
io_uring_register_files(ring, fds, MAX_CONNS);

// 클라이언트 fd 슬롯 할당
fds[slot] = client_fd;
io_uring_register_files_update(ring, slot, &client_fd, 1);

// 이후 SQE에서 인덱스 사용
sqe->flags |= IOSQE_FIXED_FILE;
sqe->fd = slot;  // fd가 아닌 registered index
```

**효과**: 고연결 환경에서 fd 조회 비용 제거. `io_uring::file`이 이를 관리.

### 2.5 Linked SQEs (원자적 연산 체인) ★★

```cpp
// 읽기 → 쓰기를 원자적으로 연결
// 읽기 실패 시 쓰기 자동 취소
io_uring_prep_read(sqe1, in_fd, buf, len, 0);
sqe1->flags |= IOSQE_IO_LINK;

io_uring_prep_write(sqe2, out_fd, buf, len, 0);
// sqe2는 sqe1 완료 후 자동 실행

// 강한 링크 (에러에도 계속): IOSQE_IO_HARDLINK
```

**적용처**: TCP 프록시, 파이프 전달, 헤더+바디 연속 송신.

### 2.6 SQPOLL (커널 폴링 스레드) ★★

```
기존:  io_uring_enter() syscall → 커널에 SQE 통지
SQPOLL: 커널 스레드가 SQE 링을 폴링 → io_uring_enter() 불필요
        (유저는 SQE만 채우면 됨, syscall 없음)
```

```cmake
# CMake option
option(QBUEM_IOURING_SQPOLL "Enable io_uring SQPOLL mode" OFF)
# root 권한 또는 CAP_SYS_NICE 필요
# 저부하 시 CPU 낭비 → 고부하 서버에서만 활성화
```

현재 `IOUringReactor`에 구현됨 (`is_sqpoll()` 확인 가능). **권한 있으면 자동 활성화.**

### 2.7 Zero-copy Send: `IORING_OP_SENDMSG_ZC` (Linux 6.0+) ★★

```
기존:  send(fd, buf) → 커널: user buf → kernel buf 복사 → NIC
ZC:    SQE(SENDMSG_ZC) → 커널: user buf 직접 NIC → CQE(completion 알림)
       복사 없음. buf는 completion 전까지 수정 불가.
```

```cpp
// qbuem::zerocopy 내부 구현 (io_uring 경로)
struct io_uring_zc_tx_notification {
    // completion notification
};

io_uring_prep_sendmsg_zc(sqe, fd, &msg, 0);
// CQE.flags & IORING_CQE_F_NOTIF → notification CQE 추가 예정
// 두 번째 CQE(notification)가 와야 buf 해제 가능
```

### 2.8 `IORING_OP_SOCKET` + `IORING_OP_CONNECT` (Linux 5.19+)

소켓 생성과 연결도 io_uring으로:

```cpp
// 소켓 생성 (syscall 없음)
io_uring_prep_socket(sqe, AF_INET, SOCK_STREAM, 0, 0);

// 연결 (linked sqe)
sqe->flags |= IOSQE_IO_LINK;
io_uring_prep_connect(sqe2, fd_from_cqe, addr, addrlen);
```

**적용처**: `ConnectionPool::acquire()` — 새 연결 생성 경로.

### 2.9 `IORING_OP_FUTEX_WAIT/WAKE` (Linux 6.7+) ★

현재 `Reactor::post()` wakeup 경로:
```
eventfd write → epoll_wait 조기 반환
```

futex 기반으로 교체 시:
```cpp
// eventfd 없이 futex로 wakeup
// 더 빠른 wakeup latency
io_uring_prep_futex_wait(sqe, &wake_flag, 0, FUTEX_BITSET_MATCH_ANY, ...);
// wakeup:
io_uring_prep_futex_wake(sqe, &wake_flag, 1, FUTEX_BITSET_MATCH_ANY, ...);
```

### 2.10 CQE32 (128-byte CQE) (Linux 6.0+)

```cpp
// 기본 CQE: 16 bytes (user_data, res, flags)
// CQE32:  128 bytes — 추가 메타데이터 (TCP 소스 포트, 타임스탬프 등)
// setup flag: IORING_SETUP_CQE32
// 적용처: 정밀 타이밍 측정, 멀티경로 TCP
```

---

## 3. Kernel TLS (kTLS) ★★★

### 3.1 개념

```
기존 TLS:
  recv(fd) → user: TLS decrypt → plaintext buf
  plaintext buf → user: TLS encrypt → send(fd)
  (모든 암호화/복호화가 유저 스페이스)

kTLS (Linux 4.13+):
  recv(fd) → 커널: TLS decrypt → plaintext → user
  user: plaintext → 커널: TLS encrypt → send(fd)
  (암호화 커널, 복사 최소화)
```

### 3.2 TLS offload + kTLS flow

```cpp
// 1. 유저스페이스 TLS 구현체로 핸드셰이크
auto tls_ctx = OpenSSLTransport::handshake(fd);

// 2. 협상된 키/IV를 커널에 전달
struct tls12_crypto_info_aes_gcm_128 info = {
    .info = { .version = TLS_1_2_VERSION, .cipher_type = TLS_CIPHER_AES_GCM_128 },
    // iv, key, rec_seq 등 핸드셰이크에서 추출
};
setsockopt(fd, SOL_TLS, TLS_TX, &info, sizeof(info));  // 송신 오프로드
setsockopt(fd, SOL_TLS, TLS_RX, &info, sizeof(info));  // 수신 오프로드

// 3. 이후 send()/recv()는 커널이 자동으로 암/복호화
// sendfile()과 결합 → 파일 → TLS → NIC (완전 zero-copy)
```

**`qbuem::transport` 통합:**
```cpp
class kTLSTransport final : public ITransport {
    // OpenSSL로 핸드셰이크
    // 키 추출 → setsockopt(SOL_TLS)
    // 이후 PlainTransport처럼 동작 (커널이 암호화)
    // sendfile과 결합 가능
};
```

**효과**: TLS 연결에서도 `sendfile()` zero-copy 사용 가능.

---

## 4. TCP 고급 옵션

### 4.1 현재 구현된 옵션 (완료)

| 옵션 | 효과 |
|------|------|
| `TCP_NODELAY` | Nagle 알고리즘 비활성화 → 소패킷 즉시 전송 |
| `TCP_CORK` | 버퍼 가득 찰 때만 전송 → writev와 조합 |
| `TCP_QUICKACK` | delayed ACK 비활성화 → RTT 최소화 |
| `SO_REUSEPORT` | reactor당 독립 소켓 → accept 경합 없음 |
| `SO_BUSY_POLL` | 수신 바쁜 대기 → 초저지연 (CPU trade-off) |
| `TCP_FASTOPEN` | SYN+데이터 → 1-RTT 단축 |
| `TCP_DEFER_ACCEPT` | 데이터 도착 후 accept → 빈 연결 방지 |
| `SO_SNDTIMEO` | 느린 클라이언트 차단 |

### 4.2 추가 예정 옵션

#### `SO_INCOMING_CPU` (Linux 3.19+) ★

```cpp
// 연결을 특정 CPU의 reactor에 고정
// SO_REUSEPORT와 조합하면 CPU별 연결 분리 완성
int cpu = worker_idx;  // reactor N → CPU N
setsockopt(listen_fd, SOL_SOCKET, SO_INCOMING_CPU, &cpu, sizeof(cpu));
```

**효과**: 연결이 항상 같은 CPU로 → L1/L2 캐시 적중률 극대화.

#### `TCP_MIGRATE_REQ` (Linux 5.14+) ★

```cpp
// SO_REUSEPORT 그룹 내에서 연결을 다른 소켓으로 마이그레이션
// hot reload 시: 새 소켓 그룹 생성 → 연결 이관 → 구 소켓 닫기
setsockopt(new_listen_fd, SOL_TCP, TCP_MIGRATE_REQ, &1, sizeof(int));
```

**적용처**: 무중단 재시작(graceful restart), worker 재배치.

#### `TCP_ULP` (Linux 4.13+) — kTLS 진입점

```cpp
// kTLS 활성화 전 필수
setsockopt(fd, SOL_TCP, TCP_ULP, "tls", sizeof("tls"));
// 이후 setsockopt(SOL_TLS, TLS_TX, ...) 가능
```

#### `SO_ZEROCOPY` (Linux 4.14+)

```cpp
// MSG_ZEROCOPY 사용 활성화
setsockopt(fd, SOL_SOCKET, SO_ZEROCOPY, &1, sizeof(int));
// 이후 send(fd, buf, len, MSG_ZEROCOPY) 사용 가능
// 완료는 errqueue에서 poll
```

---

## 5. 메모리 서브시스템

### 5.1 Huge Pages (2MB/1GB 페이지) ★★

```
일반 페이지: 4KB → 4MB 버퍼 풀 → TLB 엔트리 1024개 필요
Huge Pages:  2MB → 4MB 버퍼 풀 → TLB 엔트리 2개만 필요

TLB miss는 캐시 미스보다 비싸다 → 고스루풋 IO에서 중요
```

```cpp
// qbuem::arena 확장
class HugePageArena {
    static void* alloc_huge(size_t size) noexcept {
        void* p = mmap(nullptr, size,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                       -1, 0);
        if (p == MAP_FAILED) {
            // 폴백: 일반 mmap
            p = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        }
        return p;
    }
};

// BufferPool에 적용: 2MB Huge Page에 배치
template<size_t BufSize, size_t Count>
class HugeBufferPool : public BufferPool<BufSize, Count> {
    // 내부 storage가 mmap(MAP_HUGETLB)
};
```

### 5.2 `madvise` 기반 최적화

```cpp
// 수신 버퍼를 순차 접근으로 표시 → 프리페치 힌트
madvise(buf_pool, pool_size, MADV_SEQUENTIAL);

// 사용 후 페이지 재활용 (연결 종료 시)
madvise(conn_buf, buf_size, MADV_FREE);  // lazy 반환

// 핵심 버퍼는 메모리 고정 (스왑 방지)
mlock(critical_buf, size);
```

### 5.3 NUMA-aware 메모리 배치 ★★

```
NUMA 시스템: CPU 0-7 → Node 0 메모리, CPU 8-15 → Node 1 메모리
원격 메모리 접근: +100ns 패널티

전략: reactor N이 Node N의 메모리를 사용
```

```cpp
// qbuem::dispatcher 확장
class NUMADispatcher : public Dispatcher {
    void spawn_worker(size_t idx) override {
        int numa_node = numa_node_of_cpu(cpu_id_for(idx));
        // Arena를 해당 NUMA 노드에서 할당
        auto arena = Arena::alloc_on_numa(numa_node, 64 * 1024);
        // 스레드를 해당 CPU에 고정
        cpu_set_t cpuset;
        CPU_SET(cpu_id_for(idx), &cpuset);
        pthread_setaffinity_np(thread, sizeof(cpuset), &cpuset);
    }
};

// Arena에서 NUMA 인식 mmap
static void* mmap_on_numa(size_t size, int numa_node) {
    void* p = mmap(nullptr, size, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    numa_tonode_memory(p, size, numa_node);  // mbind(2) 래퍼
    return p;
}
```

### 5.4 Prefetch 힌트 ★

```cpp
// 연결 처리 루프에서 다음 Connection 구조체 미리 로드
void process_connections(Connection* conns[], size_t n) {
    for (size_t i = 0; i < n; ++i) {
        // 2개 앞 prefetch
        if (i + 2 < n)
            __builtin_prefetch(conns[i + 2], 0, 1);
        process_one(conns[i]);
    }
}

// Cache-line 정렬 보장
struct alignas(64) ConnectionSlot { Connection conn; };
```

### 5.5 mmap 기반 Arena (OS 반환 없는 버전)

```cpp
// 현재 Arena: malloc 기반 블록
// 개선: mmap 기반 → madvise(MADV_FREE)로 lazy 반환
class MmapArena {
    void* mmap_alloc(size_t size) {
        return mmap(nullptr, size,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS,
                    -1, 0);
    }
    void reset() {
        // OS에 메모리 반환하지 않고 페이지만 초기화
        madvise(base_, size_, MADV_DONTNEED);
        // 다음 write 시 zero-fill → allocation은 유지
    }
};
```

---

## 6. Zero-copy 심층

### 6.1 `sendfile(2)` — 정적 파일 (이미 구현)

```
파일 → 소켓 direct transfer
kernel: file buf → socket buf (복사 1회)
DMA 지원 NIC: file → NIC (복사 0회)
```

**kTLS 연동**: `setsockopt(SOL_TLS)` 후 `sendfile()` → 커널이 TLS 암호화하며 전송.

### 6.2 `splice(2)` — Generic Pipe 기반

```cpp
// pipe를 intermediary로 사용한 zero-copy fd→fd
int pipe_fds[2];
pipe2(pipe_fds, O_NONBLOCK);

// 소스 fd에서 pipe로
splice(src_fd, nullptr, pipe_fds[1], nullptr, len,
       SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

// pipe에서 목적지 fd로
splice(pipe_fds[0], nullptr, dst_fd, nullptr, len,
       SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
```

**적용처**: 프록시 서버 (in_sock → out_sock), 스트리밍 전달.

### 6.3 `MSG_ZEROCOPY` — 동적 응답 (구현 예정)

```cpp
// 송신 버퍼가 NIC DMA까지 직접 매핑
// 완료 알림이 errqueue로 비동기 통지

send(fd, buf, len, MSG_ZEROCOPY);

// 완료 대기 (별도 코루틴 또는 Reactor 통합)
struct msghdr msg = {};
char ctrl[CMSG_SPACE(sizeof(struct sock_extended_err))];
msg.msg_control = ctrl;
msg.msg_controllen = sizeof(ctrl);
recvmsg(fd, &msg, MSG_ERRQUEUE);  // completion 수신
```

**trade-off**: 작은 메시지(<4KB)는 복사가 더 빠를 수 있음 → 임계값 측정 필요.

### 6.4 `io_uring` + `MSG_ZEROCOPY` 조합

```cpp
// io_uring IORING_OP_SENDMSG_ZC (Linux 6.0+)
io_uring_prep_sendmsg_zc(sqe, fd, &msg, flags);

// 두 개의 CQE 생성:
// CQE1: 전송 시작 (res = bytes)
// CQE2: 완료 notification (flags & IORING_CQE_F_NOTIF)
// CQE2 수신 후에야 버퍼 해제 가능
```

### 6.5 GRO (Generic Receive Offload) 인식

```
GRO: NIC가 여러 패킷을 하나로 합침 → recv()에서 큰 버퍼 수신
qbuem 대응: ReadBuf<N>을 충분히 크게 (N=65536)
            LRO(Large Receive Offload) 활성화 NIC에서 효과적
```

---

## 7. AF_XDP (eXpress Data Path) ★★★

### 7.1 개념

```
일반 경로:  NIC → 드라이버 → sk_buff → 소켓 버퍼 → recv() → 유저
AF_XDP:     NIC → XDP 프로그램 → UMEM(유저 메모리) → 유저
            (커널 네트워크 스택 완전 우회)
```

**성능 목표**: 10-100M PPS, sub-microsecond latency

### 7.2 컴포넌트

```
UMEM: 유저가 할당한 메모리 영역 (패킷 버퍼)
Fill Queue (FQ): 유저가 빈 버퍼 슬롯 제공
Completion Queue (CQ): 전송 완료된 버퍼 반환
RX Queue: 수신 패킷 (UMEM 슬롯 인덱스)
TX Queue: 전송할 패킷 (UMEM 슬롯 인덱스)
XDP 프로그램: 패킷 필터링 결정 (BPF bytecode)
```

### 7.3 qbuem 통합 계획 (별도 레이어)

```cpp
// qbuem::xdp — 선택적 별도 라이브러리
// 의존: qbuem::buf (UMEM을 BufferPool로 관리)

namespace qbuem::xdp {

struct UmemConfig {
    size_t frame_size   = 4096;   // 반드시 2의 거듭제곱
    size_t frame_count  = 4096;   // 총 프레임 수 (16MB = 4096 * 4096)
    bool   huge_pages   = true;   // 2MB huge pages 사용
};

class XdpSocket {
public:
    static Result<XdpSocket> bind(std::string_view ifname,
                                   uint32_t queue_id,
                                   UmemConfig cfg);

    // zero-copy recv: 슬롯 인덱스 → UMEM 직접 포인터
    std::span<const std::byte> recv_frame(uint64_t addr) noexcept;
    void recycle_frame(uint64_t addr) noexcept;

    // zero-copy send
    Result<void> send_frame(uint64_t addr, size_t len) noexcept;
    void completion_collect() noexcept;
};

// XDP 프로그램 로드 (BPF)
Result<int> load_xdp_prog(std::string_view ifname,
                           std::string_view bpf_obj_path,
                           uint32_t xdp_flags = XDP_FLAGS_SKB_MODE);

} // namespace qbuem::xdp
```

**적용처**: 게임 서버 (UDP/QUIC), 고속 프록시, 패킷 캡처.

---

## 8. 프로토콜 심층

### 8.1 QUIC / HTTP/3

```
HTTP/3 = HTTP over QUIC (UDP 기반)
QUIC 특징:
  - 0-RTT 연결 (재연결 시)
  - 독립 스트림 (HOL blocking 없음)
  - 연결 마이그레이션 (IP 변경 시 연결 유지)
  - 내장 TLS 1.3

커널 QUIC: Linux 6.x 계획 (아직 미완성)
현재 옵션: quiche (Cloudflare), ngtcp2, msquic (MS)
```

**qbuem 통합 전략:**
```cpp
// quiche FFI (zero-copy, Rust 라이브러리)
// quiche_conn은 상태 머신 — IO는 qbuem::net이 처리
class QuicheTransport final : public ITransport {
    quiche_conn* conn_;
    TcpStream    udp_sock_;  // 실제로는 UdpSocket
public:
    Task<Result<size_t>> read (std::span<std::byte>)       override;
    Task<Result<size_t>> write(std::span<const std::byte>) override;
    Task<Result<void>>   handshake()                       override;
};
```

### 8.2 HTTP/2 HPACK (zero-alloc 구현)

```
HPACK: 정적 테이블(61개 사전 정의 헤더) + 동적 테이블(연결당 상태)
zero-alloc 구현 전략:
  - 정적 테이블: const 배열 (컴파일타임)
  - 동적 테이블: Arena 기반 순환 버퍼
  - 헤더 값: ReadBuf 내부 슬라이스 (복사 없음)
```

```cpp
class HpackDecoder {
    Arena& arena_;
    // 동적 테이블: Arena에서 할당, 엔트리 추방 시 Arena::reset 불가
    // → 별도 FixedPoolResource<HeaderEntry> 사용
    FixedPoolResource<sizeof(HpackEntry)> table_{256};
    HpackEntry* table_[256];
    size_t table_size_ = 0;
    size_t table_used_ = 0;

public:
    // 디코드: IOSlice 반환 (arena 또는 ReadBuf 내부 포인터)
    Result<std::span<HpackHeader>>
    decode(std::span<const std::byte> input, Arena& arena) noexcept;
};
```

### 8.3 WebSocket 최적화

```
현재 구현 계획: 표준 RFC 6455
추가 최적화:
  - Permessage-deflate 압축 (IBodyEncoder 통해 주입)
  - 바이너리 프레임 zero-copy (ReadBuf 슬라이스 직접 전달)
  - Server-side masking 우회 (서버는 masking 안 해도 됨)
  - WSS: kTLS 연동 → TLS 암호화 offload
```

---

## 9. 성능 프로파일링 도구 통합

### 9.1 Perf 이벤트 (Linux)

```cpp
// qbuem::metrics 확장: PMU(Performance Monitoring Unit) 카운터
class PerfCounters {
    int fds_[4];  // PERF_TYPE_HARDWARE 이벤트들
public:
    void start() noexcept;
    struct Sample {
        uint64_t cycles;         // CPU 사이클
        uint64_t instructions;   // 명령어 수
        uint64_t cache_misses;   // LLC miss
        uint64_t branch_misses;  // 분기 예측 실패
    };
    Sample read() noexcept;
};
```

### 9.2 eBPF 트레이싱

```
qbuem + eBPF 시나리오:
  - kprobe on tcp_sendmsg → 실제 전송 지연 측정
  - uprobe on ITransport::write() → 암호화 시간
  - tracepoint:net:net_dev_xmit → NIC 전송 완료
  - io_uring tracepoints → SQE/CQE latency
```

### 9.3 PGO (Profile-Guided Optimization)

```cmake
# 2-pass PGO 빌드
# Pass 1: 프로파일링 빌드
target_compile_options(qbuem_http PRIVATE -fprofile-generate)
# → wrk로 10분 워밍업

# Pass 2: 최적화 빌드
target_compile_options(qbuem_http PRIVATE
    -fprofile-use
    -fprofile-correction)  # 멀티스레드 카운터 오차 보정
```

---

## 10. 고급 기술 로드맵

| 기술 | 난이도 | 효과 | 버전 | 의존 커널/라이브러리 |
|------|--------|------|------|---------------------|
| io_uring 직접 RECV/SEND SQE | ★★★ | ★★★ | v0.7.0 | Linux 5.1+ |
| `IORING_OP_ACCEPT_MULTISHOT` | ★★ | ★★★ | v0.7.0 | Linux 5.19+ |
| `IORING_OP_RECV_MULTISHOT` | ★★ | ★★★ | v0.7.0 | Linux 5.19+ |
| io_uring Fixed Files | ★★ | ★★ | v0.7.0 | Linux 5.1+ |
| io_uring Linked SQEs | ★★ | ★★ | v0.7.0 | Linux 5.1+ |
| kTLS (TX/RX offload) | ★★★ | ★★★ | v0.8.0 | Linux 4.13+ |
| `SO_INCOMING_CPU` | ★ | ★★ | v0.7.0 | Linux 3.19+ |
| `TCP_MIGRATE_REQ` | ★★ | ★★ | v0.9.0 | Linux 5.14+ |
| Huge Pages 버퍼 풀 | ★★ | ★★ | v0.8.0 | Linux (always) |
| NUMA-aware Arena | ★★★ | ★★★ | v0.9.2 | Linux libnuma |
| Prefetch 힌트 | ★ | ★ | v0.9.0 | (컴파일러) |
| `MSG_ZEROCOPY` | ★★ | ★★ | v0.7.0 | Linux 4.14+ |
| `IORING_OP_SENDMSG_ZC` | ★★★ | ★★★ | v0.8.0 | Linux 6.0+ |
| `IORING_OP_FUTEX_*` | ★★ | ★ | v0.9.0 | Linux 6.7+ |
| QUIC/HTTP3 (quiche FFI) | ★★★ | ★★★ | v1.0.0 | quiche 0.20+ |
| AF_XDP + UMEM | ★★★★ | ★★★★ | v1.1.0 | Linux 4.18+ libbpf |
| HPACK zero-alloc | ★★★ | ★★ | v1.0.0 | (자체 구현) |
| PGO 2-pass 빌드 | ★★ | ★★ | v0.9.0 | (빌드 시스템) |

---

## 11. 플랫폼별 기술 매트릭스

| 기술 | Linux | macOS | FreeBSD | 비고 |
|------|-------|-------|---------|------|
| epoll | ✓ | ✗ | ✗ | Linux 전용 |
| kqueue | ✗ | ✓ | ✓ | BSD계열 |
| io_uring | ✓ | ✗ | ✗ | Linux 5.1+ |
| io_uring SQPOLL | ✓ | ✗ | ✗ | root/CAP_SYS_NICE |
| io_uring multishot | ✓ | ✗ | ✗ | Linux 5.19+ |
| SO_REUSEPORT | ✓ | ✓ | ✓ | |
| SO_INCOMING_CPU | ✓ | ✗ | ✗ | Linux 3.19+ |
| TCP_DEFER_ACCEPT | ✓ | ✗ | ✗ | |
| TCP_FASTOPEN | ✓ | ✓ | ✓ | macOS 10.11+ |
| TCP_MIGRATE_REQ | ✓ | ✗ | ✗ | Linux 5.14+ |
| kTLS | ✓ | ✗ | ✗ | Linux 4.13+ |
| sendfile | ✓ | ✓ | ✓ | API 다름 |
| splice | ✓ | ✗ | ✗ | Linux 전용 |
| MSG_ZEROCOPY | ✓ | ✗ | ✗ | Linux 4.14+ |
| Huge Pages (MAP_HUGETLB) | ✓ | ✗ | ✓ | |
| AF_XDP | ✓ | ✗ | ✗ | Linux 4.18+ |
| QUIC (kernel) | 미완 | ✗ | ✗ | 개발 중 |

**macOS 전략**: kqueue + sendfile + SO_REUSEPORT 조합으로 커버.
고성능 기능(io_uring, kTLS, MSG_ZEROCOPY)은 Linux 전용. macOS는 개발 환경용.

---

## 12. IO 레이어 성능 측정 방법

### 12.1 마이크로 벤치마크

```cpp
// 각 IO 기술별 분리 측정
// Arena: allocate() 속도 → 10ns 이하 목표
// ReadBuf: commit/consume → 1ns 이하 (포인터 연산만)
// TcpStream::read() → syscall 오버헤드 측정
// io_uring SQE 제출 latency → SQPOLL vs 일반 모드 비교
```

### 12.2 실부하 측정

```bash
# HTTP/1.1 처리량
wrk2 -t 8 -c 1000 -d 30s -R 500000 http://localhost:8080/ping

# Latency 분포
wrk2 --latency -t 8 -c 1000 -d 30s http://localhost:8080/api

# 연결 수락율
# custom: TCP SYN flood → accept 처리 속도

# Allocation 검증
LD_PRELOAD=libjemalloc.so MALLOC_CONF=stats_print:true ./server
# hot path에서 malloc 호출 없어야 함
```

### 12.3 Flamegraph

```bash
# perf 기반 flamegraph
perf record -F 999 -g ./server &
wrk2 ...
perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg
# IO 관련 함수 비율 확인: syscall vs 처리 코드 비율
```
