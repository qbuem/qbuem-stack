# qbuem Pipeline System — 기술 설계 문서

> **버전**: 0.1.0-draft
> **대상 브랜치**: `claude/continue-work-p2NeA`
> **검토 기준**: 기존 코드베이스 전체 분석 + 분산 시스템 / C++20 코루틴 / Lock-free / 시스템 설계 관점 교차 검토

---

## 목차

1. [요구사항](#1-요구사항)
2. [코드베이스 분석 — 전제 조건 및 제약](#2-코드베이스-분석--전제-조건-및-제약)
3. [이전 검토에서 수정된 기술적 오류](#3-이전-검토에서-수정된-기술적-오류)
4. [핵심 추상화 계층](#4-핵심-추상화-계층)
5. [Layer 0: Reactor::post() — 가장 중요한 인프라 전제조건](#5-layer-0-reactorpost--가장-중요한-인프라-전제조건)
6. [Layer 1: AsyncChannel](#6-layer-1-asyncchannelt)
7. [Layer 2: Stream](#7-layer-2-streamt)
8. [Layer 3: Action](#8-layer-3-actionin-out)
9. [Layer 4: Pipeline](#9-layer-4-pipelinein-out)
10. [Layer 5: PipelineGraph](#10-layer-5-pipelinegraph)
11. [메시지 패턴 (gRPC 스타일)](#11-메시지-패턴-grpc-스타일)
12. [복원력 패턴](#12-복원력-패턴)
13. [관찰 가능성 (Observability)](#13-관찰-가능성-observability)
14. [추가 개념 — 놓쳤던 부분](#14-추가-개념--놓쳤던-부분)
15. [파일 구조](#15-파일-구조)
16. [구현 순서 (의존성 그래프)](#16-구현-순서-의존성-그래프)

---

## 1. 요구사항

| # | 요구사항 | 비고 |
|---|----------|------|
| R1 | 파이프라인 = Action의 체인 | 각 Action이 독립적으로 처리 단위 |
| R2 | Action 단위 scale-in / scale-out | 워커 수를 런타임에 조정 가능 |
| R3 | 파이프라인 간 메시지 송수신 | 이름 기반 채널 레지스트리 |
| R4 | gRPC 스타일 메시지 패턴 | Unary, Server-stream, Client-stream, Bidi-stream |
| R5 | 파이프라인 완료 → 후속 파이프라인 트리거 | fan-out, fan-in, 조건부 포함 |

---

## 2. 코드베이스 분석 — 전제 조건 및 제약

### 2.1 Task<T> 특성 (task.hpp)

```
initial_suspend() → std::suspend_always     ← Lazy start (직접 resume() 필요)
unhandled_exception() → std::terminate()    ← ★ CRITICAL: 예외 = 프로세스 종료
promise.continuation = single handle<>     ← 단일 연속(single-continuation)
```

**파이프라인 영향**:
- Action 워커 함수는 반드시 `Task<Result<Out>>` 를 반환해야 함. `Task<Out>`로 하면 처리 실패 시 `std::terminate` 호출.
- 채널의 대기 큐는 `Task<T>` 외부에 별도 intrusive list 로 관리해야 함. promise에 continuation이 하나뿐이라 multiple-waiter 패턴 불가.
- 새 워커 코루틴을 spawn하면 suspended 상태로 생성됨. 올바른 reactor thread에서 `.resume()` 해야 함.

### 2.2 Reactor 특성 (reactor.hpp)

```
Shared-nothing: 각 인스턴스는 전용 스레드에서만 실행
thread-local Reactor::current() 로 접근
cross-thread 작업 주입 API 없음   ← ★ 파이프라인의 가장 큰 블로커
```

**구현된 Reactor 3종**:
- `EpollReactor`: Linux epoll + timerfd
- `IOUringReactor`: io_uring POLL_ADD + IORING_OP_TIMEOUT (Pimpl)
- `KqueueReactor`: macOS/BSD kqueue

**파이프라인 영향**:
- Action A (reactor thread 1) → Action B (reactor thread 2) 데이터 전달 시, thread 2를 깨울 수단이 없음.
- `Reactor::post(fn)` 가 없으면 cross-reactor 채널이 원천적으로 불가능.

### 2.3 Dispatcher 특성 (dispatcher.hpp)

```
register_listener(fd, cb)           ← fd 이벤트 등록만 가능
get_worker_reactor(fd)              ← fd % thread_count 해시로 reactor 반환
post_to(idx, fn) 없음               ← 특정 reactor에 작업 주입 불가
```

### 2.4 Result<T> 에러 타입 (common.hpp)

```
variant<monostate, T, error_code>   ← 파이프라인 전체의 에러 전달 타입
```

Action 반환 타입: `Task<Result<Out>>` (not `Task<Out>`)

### 2.5 Arena (arena.hpp)

메모리 Arena 존재. 대용량 파이프라인 아이템은 Arena 기반 할당으로 GC 압박 줄일 수 있음.

---

## 3. 이전 검토에서 수정된 기술적 오류

### ❌ 오류 1: fan_out 타입 서명

**이전**:
```cpp
Pipeline& fan_out({Pipeline<Out,?>& p1, p2, p3});  // '?'는 유효하지 않은 C++
```

**수정**:
```cpp
// Type-erased 인터페이스 필요
Pipeline& fan_out(std::vector<std::shared_ptr<IPipelineInput<Out>>> targets);
```

모든 `Pipeline<Out, AnyOut>` 은 `IPipelineInput<Out>` 을 구현. `PipelineGraph`는 이 인터페이스로 fan-out 배선.

---

### ❌ 오류 2: Cross-reactor 코루틴 재개 — 데이터 레이스

**이전 (잘못됨)**:
```cpp
// channel send() 내부
if (!wait_queue.empty()) {
    auto waiter = wait_queue.pop();
    waiter.handle.resume();   // ← 잘못됨: 다른 reactor 스레드의 coroutine을 직접 resume
}
```

**수정**:
```cpp
// waiter는 {reactor*, coroutine_handle<>} 쌍으로 저장
if (!wait_queue.empty()) {
    auto waiter = wait_queue.pop();
    waiter.reactor->post([h = waiter.handle]() { h.resume(); });
    // waiter가 속한 reactor 스레드에서 resume 실행 → 안전
}
```

`post()` 없이 직접 `resume()`하면 두 스레드가 같은 coroutine frame에 동시 접근 → 정의되지 않은 동작.

---

### ❌ 오류 3: scale-in 시 poison pill 신뢰성

**이전**:
```cpp
// poison pill N개 전송으로 N개 워커 중단
```

**문제**: MPMC에서 poison pill이 어떤 워커에 도달할지 보장 불가. 특히 여러 poison pill이 한 워커에 몰릴 수 있음.

**수정**:
```cpp
struct ActionWorkerContext {
    std::atomic<size_t> target_workers;   // 목표 워커 수
};

// 각 워커는 자신의 인덱스와 비교
Task<Result<void>> worker_loop(size_t my_index, ...) {
    while (!stop.stop_requested()) {
        if (my_index >= ctx.target_workers.load(std::memory_order_acquire))
            co_return Result<void>::ok();  // 자발적 종료
        auto item = co_await in_channel.recv();
        // ...
    }
}
```

---

### ❌ 오류 4: Unary 요청-응답 채널 설계

**이전**:
```
Channel<Req, Res>  ← 단일 채널로 양방향 처리 시도
```

**문제**: 단일 채널은 요청-응답 매핑이 불가. 동시에 여러 요청이 있을 때 어떤 응답이 어떤 요청에 대응하는지 알 수 없음.

**수정 (Envelope 패턴)**:
```cpp
template<typename Req, typename Res>
struct RequestEnvelope {
    Req request;
    std::shared_ptr<AsyncChannel<Result<Res>>> reply_channel;  // 1개의 응답만 올 채널
};

// 요청 보내는 쪽
auto reply_ch = std::make_shared<AsyncChannel<Result<Res>>>(1);
co_await request_channel.send(RequestEnvelope<Req,Res>{req, reply_ch});
auto result = co_await reply_ch->recv();

// 처리하는 쪽
auto env = co_await request_channel.recv();
auto res = co_await process(env.request);
co_await env.reply_channel->send(res);
```

---

### ❌ 오류 5: Stream<T> 와 move-only 타입

**이전**:
```cpp
AsyncChannel<std::optional<T>>  // optional<T>는 일부 연산에서 T의 copy constructible 요구
```

**수정**:
```cpp
struct StreamEnd {};

template<typename T>
using StreamItem = std::variant<T, StreamEnd>;

// AsyncChannel<StreamItem<T>> 사용
// StreamItem<T>는 T가 move-only여도 동작
```

---

### ❌ 오류 6: Task<T> lazy start와 워커 spawn

**이전**: 새 워커를 spawn하면 자동으로 실행됨을 암묵적으로 가정.

**실제**: `initial_suspend() → std::suspend_always` 이므로 suspend 상태로 생성됨. 다음 중 하나 필요:
```cpp
// 방법 A: 올바른 reactor에서 post로 kick-off
reactor->post([task = std::move(worker_task)]() mutable {
    task.detach();   // fire-and-forget으로 전환
    // resume은 detach 내부에서... 아니다, 별도로 필요
});

// 방법 B: Dispatcher에 spawn 인터페이스 추가
dispatcher.spawn(std::move(worker_task));  // 내부에서 적절한 reactor에 post

// 방법 C: 별도 SpawnAwaiter
co_await SpawnOn{reactor, std::move(worker_task)};
```

`Dispatcher::spawn(task)` 추가가 가장 깔끔.

---

## 4. 핵심 추상화 계층

```
┌──────────────────────────────────────────────────────────────┐
│ Layer 5: PipelineGraph                                        │
│   DAG 오케스트레이션, 트리거 배선, 사이클 감지, fan-out/in    │
├──────────────────────────────────────────────────────────────┤
│ Layer 4: Pipeline<In, Out>                                    │
│   Action 체인, 완료 트리거, drain, 메트릭 집계               │
├────────────────────────────┬─────────────────────────────────┤
│ Layer 3: Action<In, Out>   │  복원력: Retry, CircuitBreaker  │
│   코루틴 워커 풀, scale     │  DLQ, Bulkhead                  │
│   stop_token, 배치 옵션    │                                  │
├────────────────────────────┴─────────────────────────────────┤
│ Layer 2: Stream<T>                                            │
│   StreamItem<T> = variant<T, StreamEnd>                       │
│   async range-for 지원, fan-out stream                        │
├──────────────────────────────────────────────────────────────┤
│ Layer 1: AsyncChannel<T>                                      │
│   Lock-free MPMC ring buffer                                  │
│   Intrusive waiter list (reactor* + coroutine_handle<>)       │
│   close() / is_closed() — EOS 전파                            │
├──────────────────────────────────────────────────────────────┤
│ Layer 0: Reactor::post(fn) + Dispatcher::spawn(task)          │
│   Cross-thread 작업 주입 — 파이프라인 전체의 기반             │
│   EpollReactor: eventfd  IOUringReactor: MSG_RING  Kqueue: EVFILT_USER │
└──────────────────────────────────────────────────────────────┘
```

---

## 5. Layer 0: Reactor::post() — 가장 중요한 인프라 전제조건

**나머지 모든 것이 이 위에 올라간다.**

### 5.1 Reactor 인터페이스 추가

```cpp
class Reactor {
public:
    // ... 기존 메서드 ...

    // ★ 신규: 다른 스레드에서 이 reactor 스레드로 fn을 안전하게 주입
    virtual void post(std::function<void()> fn) = 0;
};
```

### 5.2 구현별 전략

**EpollReactor** (Linux):
```
eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC) 생성
epoll에 EPOLLIN으로 등록
post(fn):
    mutex lock → work_queue_.push_back(fn) → unlock
    eventfd_write(efd_, 1)  ← epoll_wait 깨움
poll() 루프:
    efd 이벤트 도착 시 → eventfd_read(drain) → work_queue_ flush
```

**IOUringReactor** (Linux 6.0+):
```
IORING_OP_MSG_RING 사용: 다른 ring의 CQE를 직접 트리거
또는 EpollReactor와 동일하게 eventfd 기반 폴백 (호환성)
```

**KqueueReactor** (macOS/BSD):
```
EVFILT_USER 필터: kevent(kq, {ident, EVFILT_USER, EV_ADD|EV_CLEAR, ...}, ...)
post(fn): mutex → queue → kevent(kq, {ident, EVFILT_USER, 0, NOTE_TRIGGER, ...})
```

### 5.3 Dispatcher 추가

```cpp
class Dispatcher {
public:
    // ... 기존 메서드 ...

    // ★ 신규: 임의의 워커 reactor에 작업 주입 (load balance)
    void post(std::function<void()> fn);

    // ★ 신규: 특정 인덱스의 워커에 작업 주입
    void post_to(size_t reactor_idx, std::function<void()> fn);

    // ★ 신규: coroutine Task를 적절한 reactor에서 시작
    void spawn(Task<void>&& task);
    void spawn_on(size_t reactor_idx, Task<void>&& task);
};
```

---

## 6. Layer 1: AsyncChannel<T>

### 6.1 인터페이스

```cpp
template<typename T>
class AsyncChannel {
public:
    explicit AsyncChannel(size_t capacity);  // capacity는 power-of-2 권장

    // 보내기: 가득 찼으면 co_await 대기 (back-pressure 자동 전파)
    Task<Result<void>> send(T item);

    // 받기: 비었으면 co_await 대기
    Task<Result<T>> recv();

    // try 변형 (non-blocking, 코루틴 아님)
    bool try_send(T item);              // 가득 찼으면 false
    std::optional<T> try_recv();        // 비었으면 nullopt

    // EOS 제어
    void close();                       // 채널 닫기: 이후 recv() → errc::connection_reset
    bool is_closed() const noexcept;

    // 상태 조회
    size_t size() const noexcept;
    size_t capacity() const noexcept;
    bool empty() const noexcept;
    bool full() const noexcept;
};
```

### 6.2 내부 구조

```
ring_buffer[capacity]        ← 실제 데이터 (atomic<uint64_t> head/tail)
                               Dmitry Vyukov의 MPMC bounded queue 알고리즘 적용
                               head/tail은 cache-line 분리 (false sharing 방지)

struct WaiterEntry {
    Reactor*              reactor;  // waiter가 속한 reactor
    coroutine_handle<>    handle;   // 재개할 coroutine
    T*                    slot;     // recv 대기 시 결과 저장 위치 (송신 완료 후 fill)
};

intrusive_list<WaiterEntry> recv_waiters_;  // 비어서 대기 중인 receiver들
intrusive_list<WaiterEntry> send_waiters_;  // 가득 차서 대기 중인 sender들
std::mutex                  waiter_lock_;   // waiter list 보호 (경로: 큐 비거나 찰 때만 진입)
std::atomic<bool>           closed_{false};
```

**wakeup 경로**:
```
send(item) 성공 시:
    recv_waiters_ 에 대기자 있으면:
        waiter.reactor->post([h = waiter.handle]{ h.resume(); })
    else:
        ring_buffer에 push

recv() 성공 시:
    send_waiters_ 에 대기자 있으면:
        ring_buffer에서 아이템 꺼냄 → send_waiters_[0].reactor->post(resume_sender)
    else:
        ring_buffer에서 pop
```

### 6.3 성능 고려사항

- `head_` 와 `tail_` 은 각각 별도 캐시라인에 정렬 (`alignas(64)`)
- waiter lock 진입은 큐가 가득 차거나 비어있을 때만 발생 → 정상 흐름에서 lock-free
- `try_send` / `try_recv` 는 완전 lock-free

---

## 7. Layer 2: Stream<T>

### 7.1 StreamItem 타입 (move-only 지원)

```cpp
struct StreamEnd {};   // EOS sentinel

template<typename T>
using StreamItem = std::variant<T, StreamEnd>;
```

### 7.2 인터페이스

```cpp
template<typename T>
class Stream {
public:
    // 생산자 API
    Task<Result<void>> yield(T item);    // 아이템 발행
    void finish();                        // EOS 신호

    // 소비자 API
    // 다음 아이템. std::nullopt = EOS
    Task<std::optional<T>> next();

    // range-for 지원 (C++20 coroutine ranges)
    AsyncIterator<T>  begin();
    AsyncSentinel     end();

    // 분기: 동일 stream을 여러 소비자에게 (tee)
    std::pair<Stream<T>, Stream<T>> tee();
};
```

### 7.3 내부

`Stream<T>` 는 `shared_ptr<AsyncChannel<StreamItem<T>>>` 를 공유.
`finish()` → `channel.close()` 또는 `channel.send(StreamEnd{})` 로 EOS 전파.

---

## 8. Layer 3: Action<In, Out>

### 8.1 인터페이스

```cpp
template<typename In, typename Out>
class Action {
public:
    // 처리 함수 타입: Result<Out> 반환 필수 (exception 금지)
    using Fn = std::function<Task<Result<Out>>(In, std::stop_token)>;

    struct Config {
        size_t min_workers    = 1;
        size_t max_workers    = 16;
        size_t channel_cap    = 256;    // 입력 채널 용량 (backpressure 경계)
        bool   auto_scale     = false;
        RetryPolicy   retry   = RetryPolicy::none();
        CircuitBreaker circuit_breaker;  // 기본: disabled
        bool   keyed_ordering = false;   // true면 key 기반 consistent hash 배정
    };

    explicit Action(Fn fn, Config cfg = {});

    // 런타임 스케일 조정
    // 목표 워커 수 갱신 → 각 워커가 자신의 인덱스 vs target 비교 후 자발적 종료/생성
    void scale_to(size_t n);
    void scale_in (size_t delta = 1);
    void scale_out(size_t delta = 1);
    size_t worker_count() const noexcept;

    // 메트릭
    ActionMetrics metrics() const;

    // 내부 (Pipeline이 배선에 사용)
    std::shared_ptr<AsyncChannel<In>>  input_channel();
    std::shared_ptr<AsyncChannel<Out>> output_channel();
};
```

### 8.2 워커 루프 (의사 코드)

```cpp
Task<void> worker_loop(size_t my_idx, ActionWorkerContext& ctx,
                       AsyncChannel<In>& in, AsyncChannel<Out>& out,
                       Fn fn, std::stop_source& stop_src) {
    std::stop_token stop = stop_src.get_token();

    while (!stop.stop_requested()) {
        // scale-in 체크: 목표보다 인덱스가 크면 자발적 종료
        if (my_idx >= ctx.target_workers.load(std::memory_order_acquire))
            co_return;

        auto item_result = co_await in.recv();
        if (!item_result) co_return;  // channel closed

        // Retry + Circuit Breaker 래퍼
        auto out_result = co_await ctx.retry_wrapper(fn, *item_result, stop);

        if (!out_result) {
            // 최종 실패 → DLQ
            co_await ctx.dlq_channel->send({*item_result, out_result.error()});
            ctx.metrics.errors.fetch_add(1);
            continue;
        }

        co_await out.send(std::move(*out_result));
        ctx.metrics.processed.fetch_add(1);
    }
}
```

### 8.3 Auto-scale 정책

```
check_interval: 100ms (timer 기반)

scale-out 조건: in.size() / in.capacity() > 0.75 AND worker_count < max_workers
scale-in  조건: in.size() / in.capacity() < 0.20 AND worker_count > min_workers
hysteresis: 연속 3번 조건 충족 시 스케일 (flapping 방지)
```

### 8.4 Per-key Ordering

```cpp
// Config.keyed_ordering = true 시
// key extractor 등록
action.set_key_extractor([](const In& item) -> size_t { return item.user_id; });

// 내부: dispatch 시 key % worker_count 로 특정 워커 채널에 직접 전달
// 각 워커가 개별 input channel 보유 (worker-specific channel 배열)
```

---

## 9. Layer 4: Pipeline<In, Out>

### 9.1 인터페이스

```cpp
template<typename In, typename Out>
class Pipeline : public IPipelineInput<In> {
public:
    // Action 추가 (타입 체인)
    template<typename Mid>
    Pipeline<In, Out>& add(std::shared_ptr<Action<In, Mid>> a);

    // ── 완료 트리거 ──
    // 단일 후속 파이프라인
    Pipeline& then(std::shared_ptr<IPipelineInput<Out>> next);

    // fan-out: out 아이템을 모든 대상에 복사 전달
    Pipeline& fan_out(std::vector<std::shared_ptr<IPipelineInput<Out>>> targets);

    // fan-out: 라운드로빈 분배 (동일 아이템을 하나의 타겟에만)
    Pipeline& fan_out_round_robin(std::vector<std::shared_ptr<IPipelineInput<Out>>> targets);

    // fan-in: 이 파이프라인과 다른 파이프라인의 출력을 merge해서 next에 전달
    Pipeline& merge_with(std::shared_ptr<Pipeline<In, Out>> other,
                         std::shared_ptr<IPipelineInput<Out>> next);

    // 조건부 라우팅
    Pipeline& route_if(std::function<bool(const Out&)> pred,
                       std::shared_ptr<IPipelineInput<Out>> target);

    // Tee: 메인 체인 외 side-channel에 복사 (모니터링, 로깅)
    Pipeline& tee(std::shared_ptr<AsyncChannel<Out>> side_channel);

    // ── 라이프사이클 ──
    void start(Dispatcher& dispatcher);

    // Graceful drain: 입력 닫고 in-flight 완료 후 종료
    Task<void> drain();

    void stop();  // 즉시 정지 (in-flight 버림)

    // IPipelineInput<In> 구현
    Task<Result<void>> push(In item) override;  // backpressure 전파
    bool try_push(In item) override;

    // 메트릭
    PipelineMetrics metrics() const;
};
```

### 9.2 IPipelineInput<T> 인터페이스 (타입 소거)

```cpp
template<typename T>
struct IPipelineInput {
    virtual ~IPipelineInput() = default;
    virtual Task<Result<void>> push(T item) = 0;
    virtual bool try_push(T item) = 0;
};
```

`fan_out` 배선에서 `Pipeline<Out, AnyOut>` 의 구체 타입을 모르고도 `Out` 을 전달 가능.

---

## 10. Layer 5: PipelineGraph

### 10.1 인터페이스

```cpp
class PipelineGraph {
public:
    // 파이프라인 등록
    template<typename In, typename Out>
    void add(std::string_view name, std::shared_ptr<Pipeline<In, Out>> pipeline);

    // 파이프라인 조회
    template<typename In, typename Out>
    std::shared_ptr<Pipeline<In, Out>> get(std::string_view name) const;

    // 배선: A 완료 → B 트리거
    void connect(std::string_view from, std::string_view to);

    // fan-out: A 완료 → {B, C, D} 모두 트리거
    void fan_out(std::string_view from, std::vector<std::string_view> targets);

    // start() 시 DAG 검증 (사이클 감지) 후 모든 파이프라인 시작
    Result<void> start(Dispatcher& dispatcher);

    // 모든 파이프라인 drain
    Task<void> drain_all();

    void stop_all();
};
```

### 10.2 사이클 감지

`start()` 시 Kahn's algorithm (위상 정렬 기반) 으로 DAG 검증.
사이클 발견 시 `Result::err(errc::invalid_argument)` 반환.

---

## 11. 메시지 패턴 (gRPC 스타일)

### 11.1 Unary — Envelope 패턴

```
단일 요청 → 단일 응답
```

```cpp
template<typename Req, typename Res>
struct RequestEnvelope {
    Req                                           request;
    std::shared_ptr<AsyncChannel<Result<Res>>>    reply;  // capacity=1
};

// MessageBus 등록
bus.create_unary<Req, Res>("svc-name", channel_cap);

// 요청 측
auto reply_ch = std::make_shared<AsyncChannel<Result<Res>>>(1);
co_await bus.get_unary<Req,Res>("svc-name").send({req, reply_ch});
auto result = co_await reply_ch->recv();

// 서비스 측
auto env = co_await bus.get_unary<Req,Res>("svc-name").recv();
auto res = co_await handle(env.request);
co_await env.reply->send(res);
```

### 11.2 Server Streaming

```
단일 요청 → 스트림 응답
```

```cpp
// 요청 측
auto stream_ch = std::make_shared<AsyncChannel<StreamItem<Res>>>(64);
co_await bus.get_server_stream<Req,Res>("svc-name").send({req, stream_ch});
auto stream = Stream<Res>{stream_ch};
while (auto item = co_await stream.next()) {
    process(*item);
}

// 서비스 측
auto env = co_await bus.get_server_stream<Req,Res>("svc-name").recv();
auto out = Stream<Res>{env.stream_ch};
for (auto& r : results) {
    co_await out.yield(r);
}
out.finish();
```

### 11.3 Client Streaming

```
스트림 요청 → 단일 응답
```

```cpp
// 요청 측
auto req_stream_ch = std::make_shared<AsyncChannel<StreamItem<Req>>>(64);
auto reply_ch      = std::make_shared<AsyncChannel<Result<Res>>>(1);
co_await bus.get_client_stream<Req,Res>("svc-name").send({req_stream_ch, reply_ch});
auto out = Stream<Req>{req_stream_ch};
co_await out.yield(r1);
co_await out.yield(r2);
out.finish();
auto result = co_await reply_ch->recv();
```

### 11.4 Bidirectional Streaming

```
스트림 요청 ↔ 스트림 응답 (양방향 동시)
```

```cpp
struct BidiEnvelope {
    std::shared_ptr<AsyncChannel<StreamItem<Req>>> req_stream;
    std::shared_ptr<AsyncChannel<StreamItem<Res>>> res_stream;
};

// 요청/응답 양 방향이 독립적인 코루틴에서 동시 진행
// co_await send 와 co_await next()를 서로 다른 Task 에서 수행
```

### 11.5 MessageBus

```cpp
class MessageBus {
public:
    // Unary 채널 생성/조회
    template<typename Req, typename Res>
    void   create_unary(std::string_view name, size_t cap = 64);
    template<typename Req, typename Res>
    AsyncChannel<RequestEnvelope<Req,Res>>& get_unary(std::string_view name);

    // Server/Client/Bidi stream 채널도 동일 패턴
    // ...

    // 전체 채널 목록 (디버그/메트릭)
    std::vector<std::string> channel_names() const;
};
```

---

## 12. 복원력 패턴

### 12.1 RetryPolicy

```cpp
struct RetryPolicy {
    uint32_t max_attempts = 1;  // 1 = retry 없음

    enum class BackoffKind { Fixed, Exponential, ExponentialJitter };
    BackoffKind backoff      = BackoffKind::Exponential;
    Duration    base_delay   = 100ms;
    Duration    max_delay    = 30s;
    Duration    deadline     = Duration::max();  // 절대 시간 제한

    // 재시도할 에러 코드 필터 (empty = 모두 재시도)
    std::vector<std::error_code> retryable_errors;

    static RetryPolicy none() { return {.max_attempts = 1}; }
    static RetryPolicy exponential(uint32_t n) { return {.max_attempts = n}; }
};
```

### 12.2 CircuitBreaker

```cpp
class CircuitBreaker {
public:
    // 상태: Closed(정상) → Open(차단) → HalfOpen(탐색) → Closed
    enum class State { Closed, Open, HalfOpen };

    struct Config {
        uint32_t failure_threshold   = 5;     // 연속 실패 N회 → Open
        uint32_t success_threshold   = 2;     // HalfOpen에서 연속 성공 N회 → Closed
        Duration  open_duration      = 30s;   // Open 유지 시간 후 HalfOpen
    };

    // Action 내부에서 사용
    bool allow_request() const;           // Open이면 false
    void record_success();
    void record_failure();
    State state() const noexcept;
};
```

Open 상태에서 들어온 아이템은 즉시 DLQ로 전송 (처리 시도 없음).

### 12.3 Dead Letter Queue (DLQ)

```cpp
template<typename T>
struct DeadLetter {
    T               item;
    std::error_code error;
    uint32_t        attempt_count;
    std::chrono::system_clock::time_point failed_at;
};

// MessageBus에서 이름으로 접근
auto& dlq = bus.get_dlq<MyType>("my-action-dlq");

// 소비자: 실패 아이템 검사, 재처리, 알람 등
while (auto dead = co_await dlq.recv()) {
    log_error(dead->error, dead->item);
    // 선택적: manual replay
}
```

### 12.4 Bulkhead

각 Action의 입력 채널 용량(`channel_cap`)이 자연스러운 Bulkhead.
채널이 가득 차면 upstream Action의 `send()` 가 co_await 대기 → 자동 back-pressure 전파.
`try_push()` 를 쓰는 곳에서는 명시적으로 `errc::resource_unavailable_try_again` 반환.

---

## 13. 관찰 가능성 (Observability)

### 13.1 Action 메트릭

```cpp
struct ActionMetrics {
    std::atomic<uint64_t> items_processed{0};
    std::atomic<uint64_t> items_errored{0};
    std::atomic<uint64_t> items_retried{0};
    std::atomic<uint64_t> items_dlq{0};

    // Latency 히스토그램 (버킷: < 1ms, < 10ms, < 100ms, >= 100ms)
    std::array<std::atomic<uint64_t>, 4> latency_buckets{};

    std::atomic<uint32_t> current_workers{0};
    // 채널 깊이는 channel.size()로 직접 조회
};
```

### 13.2 메트릭 수집 훅

```cpp
struct PipelineObserver {
    // Action 처리 시작/완료 시 호출
    virtual void on_item_start (std::string_view action_name) = 0;
    virtual void on_item_done  (std::string_view action_name, Duration elapsed) = 0;
    virtual void on_item_error (std::string_view action_name, std::error_code) = 0;
    virtual void on_scale_event(std::string_view action_name, size_t old_n, size_t new_n) = 0;
};
```

기본 구현: `PrometheusObserver`, `LoggingObserver`.

---

## 14. 추가 개념 — 놓쳤던 부분

### 14.1 BatchAction (★ 새로운 개념)

개별 아이템이 아닌 **묶음 단위** 처리. DB bulk insert, HTTP 배치 요청 등에 유용.

```cpp
template<typename In, typename Out>
class BatchAction {
    using BatchFn = std::function<Task<Result<std::vector<Out>>>(std::vector<In>, std::stop_token)>;

    struct BatchConfig {
        size_t   max_batch_size  = 100;
        Duration max_wait        = 10ms;  // 배치가 덜 찼어도 이 시간 후 처리
    };

    // 내부: AsyncChannel<In>에서 max_batch_size 개 또는 max_wait ms 동안 아이템 수집
    // → BatchFn 호출 → 결과 아이템들을 output channel에 개별 전송
};
```

### 14.2 Generator<T> (co_yield 기반 소스)

파이프라인에 데이터를 주입하는 경량 소스. Action이 필요 없는 단순 생산자.

```cpp
template<typename T>
struct Generator {
    struct promise_type {
        T yielded_value;
        // co_yield v → yield_value(v) → suspend
        // 다음 recv() 시 resume
        ...
    };

    // 소비 측: AsyncIterator처럼 사용
    Task<std::optional<T>> next();
};

// 사용 예시
Generator<int> counter(int n) {
    for (int i = 0; i < n; ++i)
        co_yield i;
}
pipeline.set_source(counter(1000));
```

### 14.3 Dispatcher::spawn() (★ 핵심 추가)

```cpp
// 파이프라인 워커 코루틴을 dispatcher에 spawn
// round-robin으로 reactor 선택 후 post()로 kick-off
dispatcher.spawn(std::move(worker_task));
dispatcher.spawn_on(reactor_idx, std::move(worker_task));
```

없으면 Action scale-out 시 새 워커를 어디서 어떻게 시작하는지가 불명확해짐.

### 14.4 Worker Affinity Option

```cpp
struct ActionConfig {
    // ...
    std::optional<size_t> reactor_affinity;  // 특정 reactor에 고정
    // nullopt = dispatcher가 적절히 분배
};
```

CPU 캐시 지역성이 중요한 경우 (파이프라인 전체를 같은 reactor에 핀).

### 14.5 Pipeline Snapshot / Replay

```cpp
// DLQ에 쌓인 아이템을 나중에 재처리
Task<void> replay_dlq(Pipeline<T, ?>& target, size_t max_items = SIZE_MAX);
```

---

## 15. 파일 구조

```
include/qbuem/pipeline/
├── channel.hpp          AsyncChannel<T>
├── stream.hpp           Stream<T>, StreamItem<T>
├── action.hpp           Action<In,Out>, BatchAction<In,Out>
├── pipeline.hpp         Pipeline<In,Out>, IPipelineInput<T>
├── pipeline_graph.hpp   PipelineGraph
├── message_bus.hpp      MessageBus, RequestEnvelope
├── resilience.hpp       RetryPolicy, CircuitBreaker, DeadLetter
├── metrics.hpp          ActionMetrics, PipelineMetrics, PipelineObserver
└── generator.hpp        Generator<T>

src/pipeline/
├── channel.cpp
├── action.cpp
├── pipeline.cpp
├── pipeline_graph.cpp
└── message_bus.cpp

include/qbuem/core/
├── reactor.hpp          ← post(fn) 추가
├── dispatcher.hpp       ← post(), post_to(), spawn(), spawn_on() 추가
├── epoll_reactor.hpp    ← post() 구현 (eventfd)
├── io_uring_reactor.hpp ← post() 구현 (MSG_RING 또는 eventfd 폴백)
└── kqueue_reactor.hpp   ← post() 구현 (EVFILT_USER)
```

---

## 16. 구현 순서 (의존성 그래프)

```
[0a] Reactor::post() 인터페이스 추가
[0b] EpollReactor::post() 구현 (eventfd)
[0c] IOUringReactor::post() 구현
[0d] KqueueReactor::post() 구현
[0e] Dispatcher::post() / spawn() 추가
       ↓
[1]  AsyncChannel<T>          ← [0e] 완료 후
       ↓
[2a] Stream<T>                ← [1] 완료 후
[2b] RetryPolicy / CircuitBreaker / DeadLetter (헤더 only, 의존성 없음)
       ↓
[3]  Action<In,Out>           ← [1][2a][2b] 완료 후
[3b] BatchAction<In,Out>      ← [3] 완료 후
       ↓
[4]  Pipeline<In,Out>         ← [3] 완료 후
[4b] PipelineObserver / Metrics ← [4]와 병행 가능
       ↓
[5a] PipelineGraph            ← [4] 완료 후
[5b] MessageBus               ← [1] 완료 후 (Pipeline과 독립)
       ↓
[6]  통합 테스트 + 예제
```

**최소 MVP 경로**: `[0a→0b] → [1] → [3] → [4]`
Linux 단일 플랫폼, 단일 Dispatcher 기준. 나머지는 MVP 이후 추가.

---

## 부록: 기술 결정 요약

| 결정 사항 | 채택 | 이유 |
|-----------|------|------|
| 채널 알고리즘 | Dmitry Vyukov MPMC | 검증된 알고리즘, ABA-free, cache-friendly |
| EOS 표현 | `variant<T, StreamEnd>` | move-only 타입 지원 |
| scale-in 방식 | atomic target_workers + index 비교 | poison pill보다 예측 가능 |
| 취소 방식 | `std::stop_token` | C++20 표준, 구조적 취소 |
| 에러 전파 | `Task<Result<Out>>` | exception 금지 (unhandled_exception=terminate) |
| request-reply | Envelope + per-request reply channel | 동시 요청 demux, correlation-free |
| cross-reactor wakeup | eventfd/EVFILT_USER + post() | 플랫폼별 native 메커니즘 활용 |
| fan-out 타입 소거 | `IPipelineInput<T>` 인터페이스 | heterogeneous pipeline graph 지원 |
