# qbuem-stack IO Technology Deep Dive

> **Scope**: Complete overview of currently implemented IO foundation + all applicable advanced technologies
>
> Related documents:
> - IO Layer Architecture: [docs/io-architecture.md](./io-architecture.md)
> - Library Separation Strategy: [docs/library-strategy.md](./library-strategy.md)

---

## 1. Current Implementation Status

| Technology | Implementation | Status |
|------|-----------|------|
| `epoll` (LT) | `EpollReactor` | ✓ complete |
| `kqueue` | `KqueueReactor` | ✓ complete |
| `io_uring` POLL_ADD | `IOUringReactor` | ✓ complete |
| `io_uring` SQPOLL | `IOUringReactor` | ✓ complete |
| `io_uring` Fixed Buffers | `IOUringReactor` | ✓ complete |
| `io_uring` Buffer Ring | `IOUringReactor` | ✓ complete |
| `Reactor::post()` cross-thread wakeup | all Reactors | ✓ complete |
| `SO_REUSEPORT` per-reactor | Dispatcher | ✓ complete |
| `writev()` scatter-gather | HTTP response | ✓ complete |
| `sendfile(2)` static files | StaticFiles MW | ✓ complete |
| `SO_BUSY_POLL`, `TCP_FASTOPEN` | Dispatcher | ✓ complete |
| Arena + FixedPoolResource | all layers | ✓ complete |

---

## 2. io_uring Deep Dive

### 2.1 Current: POLL_ADD-based (readiness notification)

```
[current behavior]
SQE(POLL_ADD, fd) → kernel → fd ready → CQE(fd ready) → user: perform read()/write()
                                                                ↑
                                                        this step still requires a syscall
```

The current `IOUringReactor` uses io_uring only as a **readiness notifier**.
Actual I/O is still performed via `read()`/`write()` syscalls.

### 2.2 Goal: Full io_uring (direct async I/O submission)

```
[target behavior]
SQE(RECV, fd, buf) → kernel performs recv directly and writes to buf
                   → CQE(bytes_received) → user: only processes the buffer
                                            ↑
                                    no syscall
```

**Implementation Direction:**

```cpp
// current: POLL_ADD → read() pattern
reactor->register_event(fd, EventType::Read, [fd, buf](int) {
    ssize_t n = ::read(fd, buf.data(), buf.size());  // ← syscall happens here
});

// target: submit RECV SQE directly
reactor->submit_recv(fd, buf, [](int bytes) {  // ← only SQE submission, no syscall
    // bytes: number of completed bytes
});
```

### 2.3 Multishot Operations (Linux 5.19+) ★★★

**Technology with the highest potential performance gain**

#### `IORING_OP_ACCEPT_MULTISHOT`

```
Before: accept() CQE → re-submit SQE → accept() CQE → re-submit ...
        SQE submission required per connection

Multishot: submit SQE once → CQE auto-generated per connection (no SQE re-submission)
```

```cpp
// pseudo-code
io_uring_prep_multishot_accept(sqe, listen_fd, &addr, &addrlen, 0);
// Thereafter CQE auto-generated per new connection, SQE stays active
// CQE.res = new client_fd
```

**Effect**: eliminates accept overhead on high-concurrency servers.

#### `IORING_OP_RECV_MULTISHOT` (with Buffer Ring)

```
Before:    recv() CQE → process → re-submit → recv() CQE ...
Multishot: submit SQE once → CQE auto-generated per packet
           Kernel auto-selects buffer from Buffer Ring
```

```cpp
// Register Buffer Ring
io_uring_register_buf_ring(ring, &buf_ring_reg, 0);

// Submit multishot recv (once only)
io_uring_prep_recv_multishot(sqe, client_fd, NULL, 0, 0);
sqe->buf_group = bgid;  // Buffer Ring group ID
sqe->flags |= IOSQE_BUFFER_SELECT;

// For each CQE
uint16_t buf_id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
void *data = buf_ring_get_ptr(bgid, buf_id);
size_t len = cqe->res;
// ... process ...
io_uring_buf_ring_add(...);  // return buffer
```

**Effect**: no per-connection recv SQE re-submission. Kernel auto-selects buffer, enabling zero-copy path.

### 2.4 Fixed Files (file descriptor registration) ★★

```
Before: every SQE uses fd → kernel looks up fd in file table (RCU lock)
Fixed:  io_uring_register_files() → subsequent SQEs use index, no table lookup
```

```cpp
// Register fd after accepting connection
int fds[MAX_CONNS] = {-1};
io_uring_register_files(ring, fds, MAX_CONNS);

// Allocate client fd slot
fds[slot] = client_fd;
io_uring_register_files_update(ring, slot, &client_fd, 1);

// Use index in subsequent SQEs
sqe->flags |= IOSQE_FIXED_FILE;
sqe->fd = slot;  // registered index, not fd
```

**Effect**: eliminates fd lookup cost in high-connection environments. Managed by `io_uring::file`.

### 2.5 Linked SQEs (atomic operation chaining) ★★

```cpp
// Chain read → write atomically
// If read fails, write is automatically cancelled
io_uring_prep_read(sqe1, in_fd, buf, len, 0);
sqe1->flags |= IOSQE_IO_LINK;

io_uring_prep_write(sqe2, out_fd, buf, len, 0);
// sqe2 executes automatically after sqe1 completes

// Hard link (continues even on error): IOSQE_IO_HARDLINK
```

**Use cases**: TCP proxy, pipe forwarding, consecutive header+body send.

### 2.6 SQPOLL (kernel polling thread) ★★

```
Before:  io_uring_enter() syscall → notifies kernel of SQEs
SQPOLL:  kernel thread polls SQE ring → io_uring_enter() not needed
         (user only fills SQEs, no syscall)
```

```cmake
# CMake option
option(QBUEM_IOURING_SQPOLL "Enable io_uring SQPOLL mode" OFF)
# requires root or CAP_SYS_NICE
# wastes CPU under low load → enable only on high-load servers
```

Currently implemented in `IOUringReactor` (check `is_sqpoll()`). **Automatically activated if permissions allow.**

### 2.7 Zero-copy Send: `IORING_OP_SENDMSG_ZC` (Linux 6.0+) ★★

```
Before:  send(fd, buf) → kernel: copy user buf → kernel buf → NIC
ZC:      SQE(SENDMSG_ZC) → kernel: user buf directly → NIC → CQE(completion notification)
         No copy. buf must not be modified until completion.
```

```cpp
// qbuem::zerocopy internal implementation (io_uring path)
struct io_uring_zc_tx_notification {
    // completion notification
};

io_uring_prep_sendmsg_zc(sqe, fd, &msg, 0);
// CQE.flags & IORING_CQE_F_NOTIF → additional notification CQE will follow
// buf can only be freed after the second CQE (notification) arrives
```

### 2.8 `IORING_OP_SOCKET` + `IORING_OP_CONNECT` (Linux 5.19+)

Socket creation and connection via io_uring too:

```cpp
// Socket creation (no syscall)
io_uring_prep_socket(sqe, AF_INET, SOCK_STREAM, 0, 0);

// Connection (linked sqe)
sqe->flags |= IOSQE_IO_LINK;
io_uring_prep_connect(sqe2, fd_from_cqe, addr, addrlen);
```

**Use case**: `ConnectionPool::acquire()` — new connection creation path.

### 2.9 `IORING_OP_FUTEX_WAIT/WAKE` (Linux 6.7+) ★

Current `Reactor::post()` wakeup path:
```
eventfd write → epoll_wait early return
```

When replaced with futex-based approach:
```cpp
// wakeup via futex without eventfd
// faster wakeup latency
io_uring_prep_futex_wait(sqe, &wake_flag, 0, FUTEX_BITSET_MATCH_ANY, ...);
// wakeup:
io_uring_prep_futex_wake(sqe, &wake_flag, 1, FUTEX_BITSET_MATCH_ANY, ...);
```

### 2.10 CQE32 (128-byte CQE) (Linux 6.0+)

```cpp
// Default CQE: 16 bytes (user_data, res, flags)
// CQE32:  128 bytes — additional metadata (TCP source port, timestamp, etc.)
// setup flag: IORING_SETUP_CQE32
// Use cases: precise timing measurement, multipath TCP
```

---

## 3. Kernel TLS (kTLS) ★★★

### 3.1 Concept

```
Traditional TLS:
  recv(fd) → user: TLS decrypt → plaintext buf
  plaintext buf → user: TLS encrypt → send(fd)
  (all encryption/decryption in user space)

kTLS (Linux 4.13+):
  recv(fd) → kernel: TLS decrypt → plaintext → user
  user: plaintext → kernel: TLS encrypt → send(fd)
  (encryption in kernel, copies minimized)
```

### 3.2 TLS offload + kTLS flow

```cpp
// 1. Handshake with user-space TLS implementation
auto tls_ctx = OpenSSLTransport::handshake(fd);

// 2. Pass negotiated key/IV to kernel
struct tls12_crypto_info_aes_gcm_128 info = {
    .info = { .version = TLS_1_2_VERSION, .cipher_type = TLS_CIPHER_AES_GCM_128 },
    // iv, key, rec_seq etc. extracted from handshake
};
setsockopt(fd, SOL_TLS, TLS_TX, &info, sizeof(info));  // TX offload
setsockopt(fd, SOL_TLS, TLS_RX, &info, sizeof(info));  // RX offload

// 3. Subsequent send()/recv() calls are automatically encrypted/decrypted by kernel
// combine with sendfile() → file → TLS → NIC (fully zero-copy)
```

**`qbuem::transport` integration:**
```cpp
class kTLSTransport final : public ITransport {
    // Handshake with OpenSSL
    // Extract keys → setsockopt(SOL_TLS)
    // Behaves like PlainTransport thereafter (kernel handles encryption)
    // Can be combined with sendfile
};
```

**Effect**: `sendfile()` zero-copy usable even on TLS connections.

---

## 4. TCP Advanced Options

### 4.1 Currently Implemented Options (complete)

| Option | Effect |
|------|------|
| `TCP_NODELAY` | Disables Nagle algorithm → immediate small-packet transmission |
| `TCP_CORK` | Transmit only when buffer is full → combine with writev |
| `TCP_QUICKACK` | Disables delayed ACK → minimizes RTT |
| `SO_REUSEPORT` | independent socket per reactor → no accept contention |
| `SO_BUSY_POLL` | receive busy-wait → ultra-low latency (CPU trade-off) |
| `TCP_FASTOPEN` | SYN+data → reduces by 1 RTT |
| `TCP_DEFER_ACCEPT` | accept after data arrives → prevents empty connections |
| `SO_SNDTIMEO` | blocks slow clients |

### 4.2 Planned Options

#### `SO_INCOMING_CPU` (Linux 3.19+) ★

```cpp
// Pin connection to a specific CPU's reactor
// Combined with SO_REUSEPORT, achieves per-CPU connection isolation
int cpu = worker_idx;  // reactor N → CPU N
setsockopt(listen_fd, SOL_SOCKET, SO_INCOMING_CPU, &cpu, sizeof(cpu));
```

**Effect**: connections always land on the same CPU → maximizes L1/L2 cache hit rate.

#### `TCP_MIGRATE_REQ` (Linux 5.14+) ★

```cpp
// Migrate connection to another socket within the SO_REUSEPORT group
// On hot reload: create new socket group → migrate connections → close old sockets
setsockopt(new_listen_fd, SOL_TCP, TCP_MIGRATE_REQ, &1, sizeof(int));
```

**Use case**: graceful restart, worker rebalancing.

#### `TCP_ULP` (Linux 4.13+) — kTLS entry point

```cpp
// Required before enabling kTLS
setsockopt(fd, SOL_TCP, TCP_ULP, "tls", sizeof("tls"));
// setsockopt(SOL_TLS, TLS_TX, ...) can be used afterwards
```

#### `SO_ZEROCOPY` (Linux 4.14+)

```cpp
// Enable MSG_ZEROCOPY usage
setsockopt(fd, SOL_SOCKET, SO_ZEROCOPY, &1, sizeof(int));
// send(fd, buf, len, MSG_ZEROCOPY) can be used afterwards
// completion polled from errqueue
```

---

## 5. Memory Subsystem

### 5.1 Huge Pages (2MB/1GB pages) ★★

```
Normal pages: 4KB → 4MB buffer pool → requires 1024 TLB entries
Huge Pages:   2MB → 4MB buffer pool → only 2 TLB entries needed

TLB misses are more expensive than cache misses → critical for high-throughput IO
```

```cpp
// qbuem::arena extension
class HugePageArena {
    static void* alloc_huge(size_t size) noexcept {
        void* p = mmap(nullptr, size,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                       -1, 0);
        if (p == MAP_FAILED) {
            // fallback: regular mmap
            p = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        }
        return p;
    }
};

// Apply to BufferPool: place on 2MB Huge Pages
template<size_t BufSize, size_t Count>
class HugeBufferPool : public BufferPool<BufSize, Count> {
    // internal storage via mmap(MAP_HUGETLB)
};
```

### 5.2 `madvise`-based Optimization

```cpp
// Mark receive buffer as sequential access → prefetch hint
madvise(buf_pool, pool_size, MADV_SEQUENTIAL);

// Recycle pages after use (on connection close)
madvise(conn_buf, buf_size, MADV_FREE);  // lazy reclaim

// Pin critical buffers in memory (prevent swapping)
mlock(critical_buf, size);
```

### 5.3 NUMA-aware Memory Layout ★★

```
NUMA system: CPU 0-7 → Node 0 memory, CPU 8-15 → Node 1 memory
Remote memory access: +100ns penalty

Strategy: reactor N uses Node N memory
```

```cpp
// qbuem::dispatcher extension
class NUMADispatcher : public Dispatcher {
    void spawn_worker(size_t idx) override {
        int numa_node = numa_node_of_cpu(cpu_id_for(idx));
        // Allocate Arena on the corresponding NUMA node
        auto arena = Arena::alloc_on_numa(numa_node, 64 * 1024);
        // Pin thread to the corresponding CPU
        cpu_set_t cpuset;
        CPU_SET(cpu_id_for(idx), &cpuset);
        pthread_setaffinity_np(thread, sizeof(cpuset), &cpuset);
    }
};

// NUMA-aware mmap for Arena
static void* mmap_on_numa(size_t size, int numa_node) {
    void* p = mmap(nullptr, size, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    numa_tonode_memory(p, size, numa_node);  // mbind(2) wrapper
    return p;
}
```

### 5.4 Prefetch Hints ★

```cpp
// Prefetch next Connection struct in the connection processing loop
void process_connections(Connection* conns[], size_t n) {
    for (size_t i = 0; i < n; ++i) {
        // prefetch 2 ahead
        if (i + 2 < n)
            __builtin_prefetch(conns[i + 2], 0, 1);
        process_one(conns[i]);
    }
}

// Ensure cache-line alignment
struct alignas(64) ConnectionSlot { Connection conn; };
```

### 5.5 mmap-based Arena (without OS reclamation)

```cpp
// Current Arena: malloc-based blocks
// Improvement: mmap-based → lazy reclaim via madvise(MADV_FREE)
class MmapArena {
    void* mmap_alloc(size_t size) {
        return mmap(nullptr, size,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS,
                    -1, 0);
    }
    void reset() {
        // Reset pages without returning memory to OS
        madvise(base_, size_, MADV_DONTNEED);
        // Zero-filled on next write → allocation remains
    }
};
```

---

## 6. Zero-copy Deep Dive

### 6.1 `sendfile(2)` — Static Files (already implemented)

```
file → socket direct transfer
kernel: file buf → socket buf (1 copy)
DMA-capable NIC: file → NIC (0 copies)
```

**kTLS integration**: after `setsockopt(SOL_TLS)`, `sendfile()` → kernel encrypts with TLS during transfer.

### 6.2 `splice(2)` — Generic Pipe-based

```cpp
// zero-copy fd→fd using pipe as intermediary
int pipe_fds[2];
pipe2(pipe_fds, O_NONBLOCK);

// from source fd to pipe
splice(src_fd, nullptr, pipe_fds[1], nullptr, len,
       SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

// from pipe to destination fd
splice(pipe_fds[0], nullptr, dst_fd, nullptr, len,
       SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
```

**Use cases**: proxy server (in_sock → out_sock), streaming forwarding.

### 6.3 `MSG_ZEROCOPY` — Dynamic Responses (planned)

```cpp
// Send buffer is mapped directly to NIC DMA
// Completion notification sent asynchronously via errqueue

send(fd, buf, len, MSG_ZEROCOPY);

// Await completion (separate coroutine or Reactor integration)
struct msghdr msg = {};
char ctrl[CMSG_SPACE(sizeof(struct sock_extended_err))];
msg.msg_control = ctrl;
msg.msg_controllen = sizeof(ctrl);
recvmsg(fd, &msg, MSG_ERRQUEUE);  // receive completion
```

**trade-off**: copying may be faster for small messages (<4KB) → threshold must be measured.

### 6.4 `io_uring` + `MSG_ZEROCOPY` combination

```cpp
// io_uring IORING_OP_SENDMSG_ZC (Linux 6.0+)
io_uring_prep_sendmsg_zc(sqe, fd, &msg, flags);

// Two CQEs generated:
// CQE1: send started (res = bytes)
// CQE2: completion notification (flags & IORING_CQE_F_NOTIF)
// buffer can only be freed after CQE2 is received
```

### 6.5 GRO (Generic Receive Offload) Awareness

```
GRO: NIC coalesces multiple packets into one → large buffer received in recv()
qbuem approach: make ReadBuf<N> large enough (N=65536)
                Effective on NICs with LRO (Large Receive Offload) enabled
```

---

## 7. AF_XDP (eXpress Data Path) ★★★

### 7.1 Concept

```
Normal path:  NIC → driver → sk_buff → socket buffer → recv() → user
AF_XDP:       NIC → XDP program → UMEM (user memory) → user
              (completely bypasses kernel network stack)
```

**Performance target**: 10-100M PPS, sub-microsecond latency

### 7.2 Components

```
UMEM: user-allocated memory region (packet buffers)
Fill Queue (FQ): user provides empty buffer slots
Completion Queue (CQ): returns buffers that have completed transmission
RX Queue: received packets (UMEM slot indices)
TX Queue: packets to transmit (UMEM slot indices)
XDP program: packet filtering decision (BPF bytecode)
```

### 7.3 qbuem Integration Plan (separate layer)

```cpp
// qbuem::xdp — optional separate library
// Depends on: qbuem::buf (manages UMEM via BufferPool)

namespace qbuem::xdp {

struct UmemConfig {
    size_t frame_size   = 4096;   // must be a power of 2
    size_t frame_count  = 4096;   // total frame count (16MB = 4096 * 4096)
    bool   huge_pages   = true;   // use 2MB huge pages
};

class XdpSocket {
public:
    static Result<XdpSocket> bind(std::string_view ifname,
                                   uint32_t queue_id,
                                   UmemConfig cfg);

    // zero-copy recv: slot index → direct UMEM pointer
    std::span<const std::byte> recv_frame(uint64_t addr) noexcept;
    void recycle_frame(uint64_t addr) noexcept;

    // zero-copy send
    Result<void> send_frame(uint64_t addr, size_t len) noexcept;
    void completion_collect() noexcept;
};

// Load XDP program (BPF)
Result<int> load_xdp_prog(std::string_view ifname,
                           std::string_view bpf_obj_path,
                           uint32_t xdp_flags = XDP_FLAGS_SKB_MODE);

} // namespace qbuem::xdp
```

**Use cases**: game servers (UDP/QUIC), high-speed proxy, packet capture.

---

## 8. Protocol Deep Dive

### 8.1 QUIC / HTTP/3

```
HTTP/3 = HTTP over QUIC (UDP-based)
QUIC features:
  - 0-RTT connection (on reconnect)
  - Independent streams (no HOL blocking)
  - Connection migration (maintains connection on IP change)
  - Built-in TLS 1.3

Kernel QUIC: planned for Linux 6.x (not yet complete)
Current options: quiche (Cloudflare), ngtcp2, msquic (Microsoft)
```

**qbuem integration strategy:**
```cpp
// quiche FFI (zero-copy, Rust library)
// quiche_conn is a state machine — IO is handled by qbuem::net
class QuicheTransport final : public ITransport {
    quiche_conn* conn_;
    TcpStream    udp_sock_;  // Actually a UdpSocket
public:
    Task<Result<size_t>> read (std::span<std::byte>)       override;
    Task<Result<size_t>> write(std::span<const std::byte>) override;
    Task<Result<void>>   handshake()                       override;
};
```

### 8.2 HTTP/2 HPACK (zero-alloc implementation)

```
HPACK: static table (61 predefined headers) + dynamic table (per-connection state)
zero-alloc implementation strategy:
  - Static table: const array (compile-time)
  - Dynamic table: Arena-based circular buffer
  - Header values: ReadBuf internal slices (no copy)
```

```cpp
class HpackDecoder {
    Arena& arena_;
    // Dynamic table: allocated from Arena; cannot Arena::reset when evicting entries
    // → use separate FixedPoolResource<HeaderEntry>
    FixedPoolResource<sizeof(HpackEntry)> table_{256};
    HpackEntry* table_[256];
    size_t table_size_ = 0;
    size_t table_used_ = 0;

public:
    // Decode: returns IOSlice (arena or ReadBuf internal pointer)
    Result<std::span<HpackHeader>>
    decode(std::span<const std::byte> input, Arena& arena) noexcept;
};
```

### 8.3 WebSocket Optimization

```
Current implementation plan: standard RFC 6455
Additional optimizations:
  - Permessage-deflate compression (injected via IBodyEncoder)
  - Binary frame zero-copy (pass ReadBuf slice directly)
  - Server-side masking bypass (server need not mask)
  - WSS: kTLS integration → TLS encryption offload
```

---

## 9. Performance Profiling Tool Integration

### 9.1 Perf Events (Linux)

```cpp
// qbuem::metrics extension: PMU (Performance Monitoring Unit) counters
class PerfCounters {
    int fds_[4];  // PERF_TYPE_HARDWARE events
public:
    void start() noexcept;
    struct Sample {
        uint64_t cycles;         // CPU cycles
        uint64_t instructions;   // instruction count
        uint64_t cache_misses;   // LLC miss
        uint64_t branch_misses;  // branch mispredictions
    };
    Sample read() noexcept;
};
```

### 9.2 eBPF Tracing

```
qbuem + eBPF scenarios:
  - kprobe on tcp_sendmsg → measure actual transmission latency
  - uprobe on ITransport::write() → encryption time
  - tracepoint:net:net_dev_xmit → NIC transmission complete
  - io_uring tracepoints → SQE/CQE latency
```

### 9.3 PGO (Profile-Guided Optimization)

```cmake
# 2-pass PGO build
# Pass 1: profiling build
target_compile_options(qbuem_http PRIVATE -fprofile-generate)
# → warm up with wrk for 10 minutes

# Pass 2: optimization build
target_compile_options(qbuem_http PRIVATE
    -fprofile-use
    -fprofile-correction)  # correct multi-threaded counter drift
```

---

## 10. Advanced Technology Roadmap

| Technology | Difficulty | Effect | Version | Required Kernel/Library |
|------|--------|------|------|---------------------|
| io_uring direct RECV/SEND SQE | ★★★ | ★★★ | v0.7.0 | Linux 5.1+ |
| `IORING_OP_ACCEPT_MULTISHOT` | ★★ | ★★★ | v0.7.0 | Linux 5.19+ |
| `IORING_OP_RECV_MULTISHOT` | ★★ | ★★★ | v0.7.0 | Linux 5.19+ |
| io_uring Fixed Files | ★★ | ★★ | v0.7.0 | Linux 5.1+ |
| io_uring Linked SQEs | ★★ | ★★ | v0.7.0 | Linux 5.1+ |
| kTLS (TX/RX offload) | ★★★ | ★★★ | v0.8.0 | Linux 4.13+ |
| `SO_INCOMING_CPU` | ★ | ★★ | v0.7.0 | Linux 3.19+ |
| `TCP_MIGRATE_REQ` | ★★ | ★★ | v0.9.0 | Linux 5.14+ |
| Huge Pages buffer pool | ★★ | ★★ | v0.8.0 | Linux (always) |
| NUMA-aware Arena | ★★★ | ★★★ | v0.9.2 | Linux libnuma |
| Prefetch hints | ★ | ★ | v0.9.0 | (compiler) |
| `MSG_ZEROCOPY` | ★★ | ★★ | v0.7.0 | Linux 4.14+ |
| `IORING_OP_SENDMSG_ZC` | ★★★ | ★★★ | v0.8.0 | Linux 6.0+ |
| `IORING_OP_FUTEX_*` | ★★ | ★ | v0.9.0 | Linux 6.7+ |
| QUIC/HTTP3 (quiche FFI) | ★★★ | ★★★ | v1.0.0 | quiche 0.20+ |
| AF_XDP + UMEM | ★★★★ | ★★★★ | v1.1.0 | Linux 4.18+ libbpf |
| HPACK zero-alloc | ★★★ | ★★ | v1.0.0 | (self-implemented) |
| PGO 2-pass build | ★★ | ★★ | v0.9.0 | (build system) |

---

## 11. Per-Platform Technology Matrix

| Technology | Linux | macOS | FreeBSD | Notes |
|------|-------|-------|---------|------|
| epoll | ✓ | ✗ | ✗ | Linux only |
| kqueue | ✗ | ✓ | ✓ | BSD family |
| io_uring | ✓ | ✗ | ✗ | Linux 5.1+ |
| io_uring SQPOLL | ✓ | ✗ | ✗ | root/CAP_SYS_NICE |
| io_uring multishot | ✓ | ✗ | ✗ | Linux 5.19+ |
| SO_REUSEPORT | ✓ | ✓ | ✓ | |
| SO_INCOMING_CPU | ✓ | ✗ | ✗ | Linux 3.19+ |
| TCP_DEFER_ACCEPT | ✓ | ✗ | ✗ | |
| TCP_FASTOPEN | ✓ | ✓ | ✓ | macOS 10.11+ |
| TCP_MIGRATE_REQ | ✓ | ✗ | ✗ | Linux 5.14+ |
| kTLS | ✓ | ✗ | ✗ | Linux 4.13+ |
| sendfile | ✓ | ✓ | ✓ | API differs |
| splice | ✓ | ✗ | ✗ | Linux only |
| MSG_ZEROCOPY | ✓ | ✗ | ✗ | Linux 4.14+ |
| Huge Pages (MAP_HUGETLB) | ✓ | ✗ | ✓ | |
| AF_XDP | ✓ | ✗ | ✗ | Linux 4.18+ |
| QUIC (kernel) | incomplete | ✗ | ✗ | in development |

**macOS strategy**: covered by kqueue + sendfile + SO_REUSEPORT combination.
High-performance features (io_uring, kTLS, MSG_ZEROCOPY) are Linux-only. macOS is for development environments.

---

## 12. IO Layer Performance Measurement

### 12.1 Micro-benchmarks

```cpp
// Isolated measurement per IO technology
// Arena: allocate() speed → target < 10ns
// ReadBuf: commit/consume → < 1ns (pointer arithmetic only)
// TcpStream::read() → measure syscall overhead
// io_uring SQE submission latency → compare SQPOLL vs normal mode
```

### 12.2 Real-load Measurement

```bash
# HTTP/1.1 throughput
wrk2 -t 8 -c 1000 -d 30s -R 500000 http://localhost:8080/ping

# Latency distribution
wrk2 --latency -t 8 -c 1000 -d 30s http://localhost:8080/api

# Connection accept rate
# custom: TCP SYN flood → accept processing speed

# Allocation verification
LD_PRELOAD=libjemalloc.so MALLOC_CONF=stats_print:true ./server
# no malloc calls expected on hot path
```

### 12.3 Flamegraph

```bash
# perf-based flamegraph
perf record -F 999 -g ./server &
wrk2 ...
perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg
# check IO function ratio: syscall vs processing code ratio
```
