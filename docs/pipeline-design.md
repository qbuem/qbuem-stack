# qbuem Pipeline System — 기술 설계 문서

> **버전**: 0.2.0-draft
> **대상 브랜치**: `claude/continue-work-p2NeA`
> **검토 관점**: 분산 시스템 / C++20 코루틴 / Lock-free / 시스템 설계 / Observability / 컴파일러 최적화

---

## 목차

1. [요구사항](#1-요구사항)
2. [코드베이스 분석 — 전제 조건 및 제약](#2-코드베이스-분석--전제-조건-및-제약)
3. [기술적 오류 수정 사항](#3-기술적-오류-수정-사항)
4. [정적 파이프라인 vs 동적 파이프라인](#4-정적-파이프라인-vs-동적-파이프라인)
5. [정적 액션 추가 vs 동적 액션 추가](#5-정적-액션-추가-vs-동적-액션-추가)
6. [파이프라인 상태 머신](#6-파이프라인-상태-머신)
7. [핵심 추상화 계층](#7-핵심-추상화-계층)
8. [Layer 0: Reactor::post() + Dispatcher::spawn()](#8-layer-0-reactorpost--dispatcherspawn)
9. [Layer 1: AsyncChannel](#9-layer-1-asyncchannelt)
10. [Layer 2: Stream](#10-layer-2-streamt)
11. [Layer 3: Action (정적 / 동적)](#11-layer-3-action-정적--동적)
12. [Layer 4: StaticPipeline / DynamicPipeline](#12-layer-4-staticpipeline--dynamicpipeline)
13. [Layer 5: PipelineGraph](#13-layer-5-pipelinegraph)
14. [메시지 패턴 (gRPC 스타일)](#14-메시지-패턴-grpc-스타일)
15. [분산 트레이싱 (OpenTelemetry 호환)](#15-분산-트레이싱-opentelemetry-호환)
16. [복원력 패턴](#16-복원력-패턴)
17. [Hot-swap Action (무중단 교체)](#17-hot-swap-action-무중단-교체)
18. [우선순위 채널](#18-우선순위-채널)
19. [Config-driven Pipeline (설정 주도)](#19-config-driven-pipeline-설정-주도)
20. [Pipeline Composition (파이프라인 합성)](#20-pipeline-composition-파이프라인-합성)
21. [Arena 통합 (제로카피 아이템 할당)](#21-arena-통합-제로카피-아이템-할당)
22. [관찰 가능성 — 메트릭](#22-관찰-가능성--메트릭)
23. [파일 구조](#23-파일-구조)
24. [구현 순서 (의존성 그래프)](#24-구현-순서-의존성-그래프)
25. [부록: 기술 결정 요약](#25-부록-기술-결정-요약)

---

## 1. 요구사항

| # | 요구사항 | 비고 |
|---|----------|------|
| R1 | 파이프라인 = Action의 체인 | 각 Action이 독립 처리 단위 |
| R2 | Action 단위 scale-in / scale-out | 워커 수를 런타임에 조정 |
| R3 | 파이프라인 간 메시지 송수신 | 이름 기반 채널 레지스트리 |
| R4 | gRPC 스타일 메시지 패턴 | Unary, Server-stream, Client-stream, Bidi-stream |
| R5 | 파이프라인 완료 → 후속 파이프라인 트리거 | fan-out, fan-in, 조건부 포함 |
| R6 | **정적 파이프라인 / 동적 파이프라인** | 컴파일타임 vs 런타임 구성 |
| R7 | **정적 / 동적 액션 추가** | 빌드타임 체인 vs 런타임 삽입/교체 |
| R8 | **분산 트레이싱** | W3C Trace Context, OpenTelemetry 호환 |
| R9 | **복원력 패턴** | Retry, CircuitBreaker, DLQ, Bulkhead |
| R10 | **무중단 액션 교체 (Hot-swap)** | 드레인 후 교체 |

---

## 2. 코드베이스 분석 — 전제 조건 및 제약

### 2.1 Task\<T\> 핵심 특성

```
initial_suspend() → std::suspend_always     Lazy: 생성 즉시 suspended
unhandled_exception() → std::terminate()   ★ CRITICAL: 예외 = 프로세스 종료
promise.continuation = 단일 handle<>       채널 wait_queue는 외부 intrusive list 필요
final_suspend() → symmetric transfer       스택 오버플로 없는 깊은 체인 가능
```

**파이프라인 규칙**:
- 모든 Action 처리 함수는 `Task<Result<Out>>` 반환 (예외 금지)
- 채널 wait_queue → `{Reactor*, coroutine_handle<>}` 쌍의 intrusive list
- 새 워커 spawn → `Dispatcher::spawn()` 경유 (suspended Task를 올바른 reactor에서 kick-off)

### 2.2 Reactor / Dispatcher 갭

```
cross-thread 작업 주입 API 없음   →  Reactor::post(fn) 추가 필수
Dispatcher::spawn() 없음         →  추가 필수
```

구현 3종:
- `EpollReactor` (Linux): eventfd 기반
- `IOUringReactor` (Linux): IORING_OP_MSG_RING 또는 eventfd 폴백
- `KqueueReactor` (macOS/BSD): EVFILT_USER 기반

### 2.3 기존 에러 타입

```cpp
Result<T>  →  variant<monostate, T, error_code>
```

파이프라인 전체에서 이 타입으로 에러를 값으로 전달.

### 2.4 Arena (arena.hpp)

```
bump-pointer 할당: O(1)
FixedPoolResource: 동일 크기 객체 free-list, O(1)
스레드-비안전 (shared-nothing 설계, reactor당 하나)
```

→ 파이프라인 아이템 래퍼(`PipelineItem<T>`)를 reactor-local Arena에서 할당 가능.

---

## 3. 기술적 오류 수정 사항

### ❌ 수정 1: fan_out 타입 서명

```cpp
// 이전 (잘못됨): ? 는 C++에서 유효하지 않음
Pipeline& fan_out({Pipeline<Out,?>& p1, p2, p3});

// 수정: 타입 소거 인터페이스
Pipeline& fan_out(std::vector<std::shared_ptr<IPipelineInput<Out>>>);
```

### ❌ 수정 2: Cross-reactor 코루틴 재개 — 데이터 레이스

```cpp
// 이전 (잘못됨): 다른 reactor 스레드의 coroutine을 직접 resume → 데이터 레이스
waiter.handle.resume();

// 수정: waiter가 속한 reactor에 post로 전달
waiter.reactor->post([h = waiter.handle]() { h.resume(); });
```

### ❌ 수정 3: Scale-in 시 poison pill 신뢰성

```cpp
// 이전: MPMC에서 어떤 워커가 pill을 받을지 보장 불가

// 수정: atomic target_workers + 워커 인덱스 비교
std::atomic<size_t> target_workers;
// 각 워커가 루프 상단에서 my_idx >= target_workers 이면 자발적 종료
```

### ❌ 수정 4: Unary 요청-응답 채널

```cpp
// 이전: Channel<Req, Res> 단일 채널 → 동시 요청 demux 불가

// 수정: Envelope + per-request reply channel
struct RequestEnvelope<Req, Res> {
    Req                                        request;
    shared_ptr<AsyncChannel<Result<Res>>>      reply;  // capacity=1
};
```

### ❌ 수정 5: Stream<T>와 move-only 타입

```cpp
// 이전: AsyncChannel<optional<T>> → optional의 일부 연산이 copy 요구

// 수정: variant로 move-only 완전 지원
struct StreamEnd {};
template<typename T>
using StreamItem = std::variant<T, StreamEnd>;
```

### ❌ 수정 6: Task<T> lazy start와 워커 spawn

```cpp
// 이전: spawn 후 자동 실행 가정

// 실제: initial_suspend=always → suspended 상태 생성
// 수정: Dispatcher::spawn()으로 올바른 reactor에서 kick-off
dispatcher.spawn(std::move(worker_task));
```

---

## 4. 정적 파이프라인 vs 동적 파이프라인

이것이 파이프라인 시스템의 가장 중요한 설계 분기점.

### 4.1 정적 파이프라인 (StaticPipeline)

**특징**: 타입 체인이 컴파일 타임에 완전히 결정. `build()` 호출 후 구조 변경 불가.

```cpp
// PipelineBuilder<In>은 add()마다 새로운 타입을 반환 (타입 체인)
auto pipeline =
    PipelineBuilder<HttpRequest>{}
        .add(auth_action)       // Action<HttpRequest, AuthedReq>  → Builder<AuthedReq>
        .add(parse_action)      // Action<AuthedReq, ParsedReq>    → Builder<ParsedReq>
        .add(process_action)    // Action<ParsedReq, Response>     → Builder<Response>
        .build();               // → StaticPipeline<HttpRequest, Response>

// 컴파일 타임 타입 체크: 타입 불일치 시 컴파일 에러
// add(Action<ParsedReq, X>) 뒤에 add(Action<Y, Z>) 시 Y != X면 에러
```

**장점**:
- 타입 불일치 → 컴파일 에러 (런타임 패닉 없음)
- 컴파일러가 전체 체인을 알아 인라이닝 / 데브버추얼라이제이션 가능
- 가상 함수 디스패치 없음

**단점**:
- 구조 변경 불가 (재컴파일 필요)
- Action 타입이 코드에 하드코딩됨

**사용 사례**: HTTP 요청 처리, 핵심 비즈니스 로직 파이프라인

```cpp
template<typename In>
class PipelineBuilder {
public:
    template<typename Out>
    PipelineBuilder<Out> add(std::shared_ptr<Action<In, Out>> action) && {
        // 기존 actions_ + 새 action을 담은 Builder<Out> 반환
        return PipelineBuilder<Out>{std::move(actions_), std::move(action)};
    }

    [[nodiscard]] StaticPipeline<OrigIn, In> build() && {
        return StaticPipeline<OrigIn, In>{std::move(actions_)};
    }
};
```

---

### 4.2 동적 파이프라인 (DynamicPipeline)

**특징**: 런타임에 Action을 추가/제거/교체 가능. 타입 소거.

```cpp
DynamicPipeline pipeline;
pipeline.add_action("parse",   make_dynamic_action(parse_fn));   // 뒤에 추가
pipeline.add_action("enrich",  make_dynamic_action(enrich_fn));
pipeline.add_action("output",  make_dynamic_action(output_fn));
pipeline.start(dispatcher);

// 런타임 구조 변경 (running 중: hot-swap만 가능)
// stopped 상태:
pipeline.insert_before("enrich", "validate", make_dynamic_action(validate_fn));
pipeline.remove_action("enrich");

// running 중:
co_await pipeline.hot_swap("enrich", make_dynamic_action(new_enrich_fn));
```

**타입 안전성**: `ActionSchema`로 런타임 호환성 검사

```cpp
struct ActionSchema {
    std::string input_type;    // typeid(In).name()
    std::string output_type;   // typeid(Out).name()
};
// add_action 시: new_action.input_type == prev_action.output_type 체크
// 불일치 → Result::err(errc::invalid_argument)
```

**장점**:
- Config 파일(JSON/YAML)에서 파이프라인 정의
- 플러그인 시스템: `.so` / `.dylib` 에서 Action 동적 로드
- Hot-swap: 재시작 없이 로직 교체
- A/B 테스트: 특정 % 트래픽에만 새 Action 적용

**단점**:
- 타입 소거 오버헤드 (가상 함수)
- 런타임 스키마 체크 필요
- 컴파일러 최적화 제한

**사용 사례**: ETL 파이프라인, 데이터 처리 워크플로우, 설정 주도 파이프라인

---

### 4.3 선택 기준

| 기준 | StaticPipeline | DynamicPipeline |
|------|----------------|-----------------|
| 타입 안전성 | 컴파일 타임 | 런타임 스키마 체크 |
| 성능 | 최대 최적화 | 가상 함수 오버헤드 |
| 유연성 | 없음 (재컴파일) | 런타임 변경 가능 |
| Config 주도 | 불가 | 가능 |
| Hot-swap | 불가 | 가능 |
| 적합 | 핵심 처리 파이프라인 | ETL, 운영 파이프라인 |

---

## 5. 정적 액션 추가 vs 동적 액션 추가

### 5.1 정적 액션 추가

컴파일 타임 체인. `PipelineBuilder::add<Out>(action)`.

```cpp
// 타입 체인이 컴파일 타임에 완전히 결정됨
auto p = PipelineBuilder<int>{}
    .add(int_to_float_action)    // Action<int, float>
    .add(float_to_str_action)    // Action<float, std::string>
    .build();
// decltype(p) = StaticPipeline<int, std::string>
```

`build()` 후에는 액션 추가/제거 불가. `[[nodiscard]]` 로 `build()` 결과 강제 사용.

---

### 5.2 동적 액션 추가 — 4가지 모드

#### 모드 A: 뒤에 추가 (Append) — stopped 상태만

```cpp
pipeline.add_action("new_step", make_dynamic_action(fn));
// 스키마 호환성 체크 후 마지막에 삽입
```

#### 모드 B: 특정 위치 삽입 (Insert) — stopped 상태만

```cpp
pipeline.insert_before("step_b", "new_step", make_dynamic_action(fn));
pipeline.insert_after ("step_a", "new_step", make_dynamic_action(fn));
```

#### 모드 C: 제거 (Remove) — stopped 상태만

```cpp
pipeline.remove_action("step_name");
// 제거 시 인접 액션의 스키마 호환성 재확인 필요
```

#### 모드 D: Hot-swap (Replace) — running 중 가능

```cpp
// 가장 복잡: 현재 액션의 in-flight 아이템을 drain 후 교체
co_await pipeline.hot_swap("step_name", make_dynamic_action(new_fn));
// 내부 절차:
//   1. 이 액션의 입력 채널을 "sealed" 상태로 전환 (새 아이템 수신 중단)
//   2. 기존 워커들이 큐 비울 때까지 대기
//   3. 새 Action으로 교체 후 워커 재시작
//   4. "sealed" 해제
```

---

## 6. 파이프라인 상태 머신

### 6.1 StaticPipeline 상태

```
     build()          start()         close()+drain     drain 완료
Created ──────► Built ──────► Running ──────────────► Draining ──────► Stopped
                                 │                                         │
                                 │ stop()                                  │
                                 └──────────────── Stopped ◄───────────────┘
                                 │
                                 ▼
                               Error  (Action 내부 치명적 오류 시)
```

### 6.2 DynamicPipeline 상태

```
         add_action()        start()
Created ─────────────► Configured ──────► Running ──────► Draining ──► Stopped
              ▲               ▲               │                            │
              │               │ restart()     │ hot_swap()                 │
              └───────────────┘               ▼                            │
                                         Reconfiguring                     │
                                              │                            │
                                              └──────────────► Running ◄───┘
```

### 6.3 상태 전이 규칙

```cpp
enum class PipelineState {
    Created,
    Configured,       // DynamicPipeline 전용: action 추가 완료
    Starting,
    Running,
    Reconfiguring,    // DynamicPipeline 전용: hot-swap 중
    Draining,
    Stopped,
    Error,
};

// 상태에 따른 허용 연산
// Created/Configured: add_action, remove_action, insert_action
// Running:            push, scale_to, hot_swap (Dynamic만), drain, stop
// Draining:           stop (강제 즉시 정지)
// Stopped:            start (재시작), add_action (Dynamic만)
```

---

## 7. 핵심 추상화 계층

```
┌──────────────────────────────────────────────────────────────────────┐
│ Layer 5: PipelineGraph                                                │
│   DAG 오케스트레이션, 트리거 배선, 사이클 감지, fan-out/in            │
├──────────────────────────────────────────────────────────────────────┤
│ Layer 4a: StaticPipeline<In,Out>   │  Layer 4b: DynamicPipeline      │
│   컴파일타임 타입 체인              │    런타임 타입 소거 체인          │
│   최대 최적화, 구조 고정            │    Hot-swap, Config 주도          │
├──────────────────────────────────┬─┴────────────────────────────────┤
│ Layer 3a: Action<In,Out> (정적)  │  Layer 3b: DynamicAction (동적)  │
│   코루틴 워커 풀, scale           │    타입 소거, ActionSchema        │
│   stop_token, Retry, CB          │    hot-swap 지원                  │
├──────────────────────────────────┴─────────────────────────────────-┤
│ Layer 2: Stream<T>   StreamItem<T> = variant<T, StreamEnd>           │
├──────────────────────────────────────────────────────────────────────┤
│ Layer 1: AsyncChannel<T>                                              │
│   MPMC ring buffer, intrusive waiter list, close()/EOS               │
│   PriorityChannel<T> (선택적 고우선순위 레인)                         │
├──────────────────────────────────────────────────────────────────────┤
│ Layer 0: Reactor::post() + Dispatcher::spawn()   ← 전제 조건         │
└──────────────────────────────────────────────────────────────────────┘

━━━ 횡단 관심사 (Cross-cutting) ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  Tracing: TraceContext → Span per item (sampled), W3C 호환
  Metrics: ActionMetrics, PipelineMetrics, PipelineObserver
  Resilience: RetryPolicy, CircuitBreaker, DeadLetterQueue
  Arena: reactor-local 아이템 할당 (제로카피)
```

---

## 8. Layer 0: Reactor::post() + Dispatcher::spawn()

### 8.1 Reactor 인터페이스 추가

```cpp
class Reactor {
public:
    // 다른 스레드에서 이 reactor 스레드로 fn을 안전하게 주입
    // 스레드 안전: 어느 스레드에서나 호출 가능
    virtual void post(std::function<void()> fn) = 0;
};
```

### 8.2 구현별 전략

**EpollReactor**:
```
생성 시: eventfd(0, EFD_NONBLOCK|EFD_CLOEXEC) 생성, epoll EPOLLIN 등록
post(fn): mutex lock → work_queue_.push_back(fn) → unlock → eventfd_write(1)
poll() 루프: efd 이벤트 → eventfd_read(drain) → work_queue_ flush
```

**IOUringReactor**:
```
Linux 6.0+: IORING_OP_MSG_RING (ring 간 직접 CQE 트리거, zero-copy)
< 6.0 폴백: EpollReactor와 동일하게 eventfd 사용
```

**KqueueReactor**:
```
EVFILT_USER 필터 등록
post(fn): mutex → queue → kevent(kq, NOTE_TRIGGER)
```

### 8.3 Dispatcher 추가 API

```cpp
class Dispatcher {
public:
    // 임의의 워커 reactor에 작업 주입 (round-robin load balance)
    void post(std::function<void()> fn);

    // 특정 인덱스의 워커에 작업 주입
    void post_to(size_t reactor_idx, std::function<void()> fn);

    // coroutine Task를 reactor에서 시작 (fire-and-forget)
    // 내부: task.detach() + reactor->post([&task]{ task.handle.resume(); })
    void spawn(Task<void>&& task);
    void spawn_on(size_t reactor_idx, Task<void>&& task);
};
```

---

## 9. Layer 1: AsyncChannel\<T\>

### 9.1 인터페이스

```cpp
template<typename T>
class AsyncChannel {
public:
    explicit AsyncChannel(size_t capacity);  // power-of-2

    Task<Result<void>> send(T item);         // backpressure: 가득 차면 co_await 대기
    Task<Result<T>>    recv();               // 비면 co_await 대기
    bool               try_send(T item);     // lock-free, 가득 차면 false
    std::optional<T>   try_recv();           // lock-free, 비면 nullopt

    void   close() noexcept;                // EOS 신호
    bool   is_closed() const noexcept;
    size_t size()     const noexcept;
    size_t capacity() const noexcept;
};
```

### 9.2 내부 구조

```
ring_buffer[capacity]          Dmitry Vyukov MPMC 알고리즘
                               head_/tail_ 각각 cache-line 분리 (alignas(64))
                               ABA-free: sequence number 사용

struct WaiterEntry {
    Reactor*           reactor;   // waiter 소속 reactor
    coroutine_handle<> handle;    // 재개할 코루틴
    T*                 out_slot;  // recv 대기 시: 아이템 저장 위치
    WaiterEntry*       next;      // intrusive linked list
};

intrusive_list<WaiterEntry> recv_waiters_;
intrusive_list<WaiterEntry> send_waiters_;
std::mutex                  waiter_lock_;   // 큐 empty/full 경계에서만 진입
std::atomic<bool>           closed_{false};
```

**정상 흐름 (큐 not-empty, not-full)**: lock-free (waiter_lock_ 불필요)
**대기 흐름**: waiter_lock_ 획득 → intrusive list 삽입 → co_await 중단
**wakeup**: `waiter.reactor->post([h]{h.resume();})` — cross-reactor 안전

### 9.3 PriorityChannel

```cpp
// 고우선순위 레인을 추가한 채널
// 내부: high_priority_ring + normal_priority_ring, 두 개 관리
// recv()는 항상 high_priority 먼저 소진 후 normal 처리

template<typename T>
class PriorityChannel {
public:
    enum class Priority { High, Normal, Low };

    Task<Result<void>> send(T item, Priority p = Priority::Normal);
    Task<Result<T>>    recv();  // High → Normal → Low 순서 보장
};
```

**스타베이션 방지**: Low가 N번 연속 skip되면 다음 recv에서 강제 처리 (aging 카운터).

---

## 10. Layer 2: Stream\<T\>

```cpp
struct StreamEnd {};

template<typename T>
using StreamItem = std::variant<T, StreamEnd>;  // move-only 타입 완전 지원

template<typename T>
class Stream {
public:
    // 생산자
    Task<Result<void>> yield(T item);
    void               finish();          // StreamEnd 전송

    // 소비자
    Task<std::optional<T>> next();        // nullopt = EOS

    // async range-for 지원
    AsyncIterator<T> begin();
    AsyncSentinel    end();

    // Tee: 동일 스트림을 두 소비자에게 분기
    std::pair<Stream<T>, Stream<T>> tee();
};
// 내부: shared_ptr<AsyncChannel<StreamItem<T>>> 공유
```

---

## 11. Layer 3: Action (정적 / 동적)

### 11.1 정적 Action\<In, Out\>

```cpp
template<typename In, typename Out>
class Action {
public:
    // Task<Result<Out>> 반환 필수 (exception 금지 — unhandled_exception=terminate)
    using Fn = std::function<Task<Result<Out>>(In, std::stop_token)>;

    struct Config {
        size_t        min_workers    = 1;
        size_t        max_workers    = 16;
        size_t        channel_cap    = 256;
        bool          auto_scale     = false;
        bool          keyed_ordering = false;   // per-key consistent hash 배정
        RetryPolicy   retry          = RetryPolicy::none();
        CircuitBreaker circuit_breaker;
    };

    explicit Action(Fn fn, Config cfg = {});

    void   scale_to(size_t n);
    void   scale_in (size_t delta = 1);
    void   scale_out(size_t delta = 1);
    size_t worker_count() const noexcept;

    shared_ptr<AsyncChannel<In>>  input_channel();
    shared_ptr<AsyncChannel<Out>> output_channel();
    ActionMetrics                 metrics() const;
};
```

**워커 루프 (의사코드)**:
```cpp
Task<void> worker_loop(size_t my_idx, ActionContext& ctx) {
    while (!ctx.stop.stop_requested()) {
        if (my_idx >= ctx.target_workers.load(acquire)) co_return; // scale-in

        auto item_r = co_await ctx.in->recv();
        if (!item_r) co_return;  // channel closed

        auto span = ctx.tracer.start_span(my_idx);  // 트레이싱

        auto out_r = co_await ctx.retry.execute(ctx.fn, *item_r, ctx.stop);

        ctx.tracer.end_span(span, out_r);

        if (!out_r) {
            co_await ctx.dlq->send({*item_r, out_r.error()});
            ctx.metrics.errors.fetch_add(1, relaxed);
            continue;
        }
        co_await ctx.out->send(std::move(*out_r));
        ctx.metrics.processed.fetch_add(1, relaxed);
    }
}
```

### 11.2 동적 DynamicAction

```cpp
class IDynamicAction {
public:
    virtual ~IDynamicAction() = default;
    virtual ActionSchema  schema()   const = 0;
    virtual std::string   name()     const = 0;

    // 타입 소거된 처리: void* in → void* out
    virtual Task<Result<void>> process_erased(
        void* in, void* out, std::stop_token) = 0;

    virtual void scale_to(size_t n) = 0;
    virtual ActionMetrics metrics() const = 0;
};

// 정적 Action을 동적으로 래핑하는 어댑터
template<typename In, typename Out>
auto make_dynamic_action(std::shared_ptr<Action<In, Out>> action)
    -> std::shared_ptr<IDynamicAction>;
```

---

## 12. Layer 4: StaticPipeline / DynamicPipeline

### 12.1 공통 인터페이스

```cpp
template<typename T>
struct IPipelineInput {
    virtual ~IPipelineInput() = default;
    virtual Task<Result<void>> push(T item) = 0;  // backpressure 전파
    virtual bool               try_push(T item) = 0;
};
```

### 12.2 PipelineBuilder (StaticPipeline 생성기)

```cpp
template<typename OrigIn, typename CurIn>
class PipelineBuilder {
public:
    // add()가 호출될 때마다 Builder의 CurIn 타입이 바뀜
    template<typename Out>
    [[nodiscard]] PipelineBuilder<OrigIn, Out>
    add(std::shared_ptr<Action<CurIn, Out>> action) &&;

    // 완성: StaticPipeline<OrigIn, CurIn> 반환
    [[nodiscard]] StaticPipeline<OrigIn, CurIn> build() &&;
};

// 진입점
template<typename In>
using PipelineBuilder = PipelineBuilder<In, In>;
```

### 12.3 StaticPipeline\<In, Out\>

```cpp
template<typename In, typename Out>
class StaticPipeline : public IPipelineInput<In> {
public:
    // 완료 트리거 배선
    StaticPipeline& then(shared_ptr<IPipelineInput<Out>> next);
    StaticPipeline& fan_out(vector<shared_ptr<IPipelineInput<Out>>> targets);
    StaticPipeline& fan_out_round_robin(vector<shared_ptr<IPipelineInput<Out>>> targets);
    StaticPipeline& route_if(function<bool(const Out&)> pred,
                              shared_ptr<IPipelineInput<Out>> target);
    StaticPipeline& tee(shared_ptr<AsyncChannel<Out>> side_channel);  // 모니터링 분기

    // 라이프사이클
    void          start(Dispatcher& d);
    Task<void>    drain();               // Graceful: input 닫고 in-flight 완료 후 종료
    void          stop();                // 즉시 정지

    // IPipelineInput<In>
    Task<Result<void>> push(In item) override;
    bool               try_push(In item) override;

    PipelineState  state()   const noexcept;
    PipelineMetrics metrics() const;
};
```

### 12.4 DynamicPipeline

```cpp
class DynamicPipeline {
public:
    // stopped 상태 전용 구조 변경
    Result<void> add_action(string_view name, shared_ptr<IDynamicAction> action);
    Result<void> insert_before(string_view ref, string_view name, shared_ptr<IDynamicAction>);
    Result<void> insert_after (string_view ref, string_view name, shared_ptr<IDynamicAction>);
    Result<void> remove_action(string_view name);

    // running 중 무중단 교체 (§17 참조)
    Task<Result<void>> hot_swap(string_view name, shared_ptr<IDynamicAction> new_action);

    // 완료 트리거 (타입 소거)
    void then(shared_ptr<IDynamicPipelineInput> next);
    void fan_out(vector<shared_ptr<IDynamicPipelineInput>> targets);

    // 라이프사이클
    Result<void> start(Dispatcher& d);
    Task<void>   drain();
    void         stop();

    PipelineState  state()   const noexcept;
    PipelineMetrics metrics() const;
};
```

---

## 13. Layer 5: PipelineGraph

```cpp
class PipelineGraph {
public:
    // 등록 (타입 소거로 Static/Dynamic 모두 보관)
    template<typename In, typename Out>
    void add(string_view name, shared_ptr<StaticPipeline<In, Out>> p);
    void add(string_view name, shared_ptr<DynamicPipeline> p);

    // 배선: 이름 기반
    Result<void> connect   (string_view from, string_view to);
    Result<void> fan_out   (string_view from, vector<string_view> targets);
    Result<void> merge_into(vector<string_view> sources, string_view target);
    Result<void> route_if  (string_view from, function<bool(...)> pred,
                            string_view true_target, string_view false_target);

    // start(): DAG 검증(Kahn's algorithm) 후 위상 정렬 순서로 시작
    Result<void> start(Dispatcher& d);

    Task<void> drain_all();
    void       stop_all();

    // A/B 라우팅: from의 x% 트래픽을 target_b로 (나머지는 target_a)
    void ab_route(string_view from, string_view target_a, string_view target_b,
                  double b_fraction);  // 0.0 ~ 1.0
};
```

---

## 14. 메시지 패턴 (gRPC 스타일)

### 14.1 Unary — Envelope 패턴

```cpp
template<typename Req, typename Res>
struct RequestEnvelope {
    Req                                        request;
    shared_ptr<AsyncChannel<Result<Res>>>      reply;   // capacity=1
};

// MessageBus 사용
bus.create_unary<Req, Res>("service-name", /*cap=*/64);

// 요청 측
auto reply_ch = make_shared<AsyncChannel<Result<Res>>>(1);
co_await bus.unary<Req,Res>("service-name").send({req, reply_ch});
auto result = co_await reply_ch->recv();

// 서비스 측
auto env = co_await bus.unary<Req,Res>("service-name").recv();
co_await env.reply->send(co_await process(env.request));
```

### 14.2 Server Streaming

```cpp
struct ServerStreamEnvelope<Req, Res> {
    Req                                         request;
    shared_ptr<AsyncChannel<StreamItem<Res>>>   response_stream;
};
```

### 14.3 Client Streaming

```cpp
struct ClientStreamEnvelope<Req, Res> {
    shared_ptr<AsyncChannel<StreamItem<Req>>>  request_stream;
    shared_ptr<AsyncChannel<Result<Res>>>      reply;
};
```

### 14.4 Bidirectional

```cpp
struct BidiEnvelope<Req, Res> {
    shared_ptr<AsyncChannel<StreamItem<Req>>>  req_stream;
    shared_ptr<AsyncChannel<StreamItem<Res>>>  res_stream;
};
// 두 방향이 독립적인 코루틴에서 동시 처리
```

### 14.5 MessageBus

```cpp
class MessageBus {
public:
    template<typename Req, typename Res>
    void create_unary      (string_view name, size_t cap = 64);
    template<typename Req, typename Res>
    void create_server_stream(string_view name, size_t cap = 64);
    template<typename Req, typename Res>
    void create_client_stream(string_view name, size_t cap = 64);
    template<typename Req, typename Res>
    void create_bidi       (string_view name, size_t cap = 64);

    // 각 채널 DLQ 접근
    template<typename T>
    AsyncChannel<DeadLetter<T>>& get_dlq(string_view name);

    vector<string> channel_names() const;
};
```

---

## 15. 분산 트레이싱 (OpenTelemetry 호환)

### 15.1 TraceContext — W3C Trace Context 표준

```cpp
// W3C traceparent: {version}-{trace_id}-{parent_id}-{flags}
struct TraceContext {
    uint8_t  trace_id[16];    // 128-bit, globally unique
    uint8_t  span_id[8];      // 64-bit, per-span unique
    uint8_t  trace_flags;     // bit 0: sampled

    static TraceContext generate();                          // 새 루트 컨텍스트 생성
    TraceContext        child_span() const;                  // 자식 span 생성
    std::string         to_traceparent() const;             // "00-{tid}-{sid}-{flags}"
    static TraceContext from_traceparent(string_view hdr);  // HTTP 헤더 파싱
};
```

### 15.2 아이템에 TraceContext 전파

파이프라인 아이템이 trace context를 운반하는 두 가지 전략:

**전략 A: 래핑 (Traced\<T\>)**
```cpp
template<typename T>
struct Traced {
    T            payload;
    TraceContext ctx;
};
// Pipeline<Traced<MyType>, Traced<MyResult>>
// 타입이 바뀌는 단점: 기존 Action 재사용 어려움
```

**전략 B: 옵셔널 사이드카 (권장)**
```cpp
// 파이프라인 내부에서 아이템과 별도로 TraceContext를 thread-local에 전파
// Action은 필요 시 Reactor::current_trace_context() 로 접근
// 타입 오염 없음, 트레이싱 비활성화 시 완전 zero-overhead
thread_local TraceContext current_trace_ctx;
```

전략 B가 qbuem-stack의 기존 thread-local 패턴 (`Reactor::current()`)과 일관성 있음.

### 15.3 샘플링 전략

```cpp
class Sampler {
public:
    virtual bool should_sample(const TraceContext& parent) const = 0;
};

class AlwaysSampler  : public Sampler { bool should_sample(...) { return true;  } };
class NeverSampler   : public Sampler { bool should_sample(...) { return false; } };

class ProbabilitySampler : public Sampler {
    double rate_;  // 0.0 ~ 1.0
    bool should_sample(const TraceContext& parent) const override;
};

class RateLimitingSampler : public Sampler {
    size_t max_per_second_;
    // token bucket 내부 구현
};

class ParentBasedSampler : public Sampler {
    // 부모가 sampled면 자식도 sampled
    bool should_sample(const TraceContext& parent) const override {
        return parent.trace_flags & 0x01;
    }
};
```

### 15.4 Span 수명과 오버헤드

```
Action 처리 시작: Span 생성 (TraceContext.child_span())
Action 처리 완료: Span 종료, exporter에 전송

오버헤드 분석:
- Sampler::should_sample() = 분기 1개 (or 간단한 atomic 카운터)
- 미샘플링 시: child_span() 미호출, Span 생성 0
- 샘플링 시: 64B span 스택 할당 + exporter 큐 push
             → exporter 큐를 buffered async queue로 만들면 hot path 영향 최소화
```

### 15.5 SpanExporter 인터페이스

```cpp
class SpanExporter {
public:
    struct SpanData {
        std::string   pipeline_name;
        std::string   action_name;
        TraceContext  context;       // span의 trace_id + span_id
        TraceContext  parent;        // parent span
        std::chrono::steady_clock::time_point start_time;
        std::chrono::steady_clock::time_point end_time;
        std::error_code error;       // 성공이면 기본값(무효)
        // Baggage / 태그는 추후 확장
    };

    virtual ~SpanExporter() = default;
    // 비동기: 내부 버퍼에 쌓고 배치로 전송
    virtual void export_span(SpanData data) = 0;
    virtual void flush() = 0;
};

// 구현체
class OtlpGrpcExporter  : public SpanExporter { ... };  // OpenTelemetry Collector
class OtlpHttpExporter  : public SpanExporter { ... };
class JaegerExporter    : public SpanExporter { ... };
class ZipkinExporter    : public SpanExporter { ... };
class LoggingExporter   : public SpanExporter { ... };  // 디버그용
class NoopExporter      : public SpanExporter {          // 트레이싱 비활성화
    void export_span(SpanData) override {}
    void flush()              override {}
};
```

### 15.6 PipelineTracer

```cpp
class PipelineTracer {
public:
    PipelineTracer(shared_ptr<Sampler>      sampler,
                   shared_ptr<SpanExporter> exporter);

    // Action 진입 시 호출: 현재 thread-local ctx의 child span 생성
    SpanHandle start_span(string_view pipeline, string_view action,
                          const TraceContext& parent);

    // Action 종료 시 호출
    void end_span(SpanHandle handle, const std::error_code& ec = {});
};

// 전역 등록 (Dispatcher당 하나)
PipelineTracer& global_tracer();
void set_global_tracer(shared_ptr<PipelineTracer> tracer);
```

### 15.7 HTTP와의 통합

기존 SSE, HTTP 미들웨어와의 연결:
```cpp
// HTTP 핸들러에서 traceparent 헤더 추출 → TraceContext 복원
// → 파이프라인 push() 시 thread-local에 설정
// → 파이프라인 완료 후 응답에 traceresponse 헤더 추가

auto ctx = TraceContext::from_traceparent(req.header("traceparent"));
Reactor::set_current_trace_context(ctx);
co_await pipeline.push(my_item);
```

---

## 16. 복원력 패턴

### 16.1 RetryPolicy

```cpp
struct RetryPolicy {
    uint32_t max_attempts = 1;  // 1 = no retry

    enum class BackoffKind { Fixed, Exponential, ExponentialJitter };
    BackoffKind backoff    = BackoffKind::Exponential;
    Duration    base_delay = 100ms;
    Duration    max_delay  = 30s;
    Duration    deadline   = Duration::max();  // 절대 시간 제한

    vector<error_code> retryable_errors;  // empty = 모두 재시도

    static RetryPolicy none()           { return {.max_attempts = 1}; }
    static RetryPolicy exponential(uint32_t n) { return {.max_attempts = n}; }
};
```

### 16.2 CircuitBreaker

```cpp
class CircuitBreaker {
public:
    enum class State { Closed, Open, HalfOpen };
    struct Config {
        uint32_t failure_threshold = 5;
        uint32_t success_threshold = 2;
        Duration open_duration     = 30s;
    };
    bool  allow_request() const;
    void  record_success();
    void  record_failure();
    State state() const noexcept;
};
// Open 상태에서 들어온 아이템 → 즉시 DLQ (처리 시도 없음)
```

### 16.3 Dead Letter Queue

```cpp
template<typename T>
struct DeadLetter {
    T               item;
    error_code      error;
    uint32_t        attempt_count;
    system_clock::time_point failed_at;
};
// MessageBus에서 이름으로 접근, DLQ 소비자가 재처리 / 알람 처리
```

### 16.4 Bulkhead

각 Action의 `channel_cap` 이 자연스러운 Bulkhead.
채널 포화 → upstream send() 가 co_await 대기 → 자동 backpressure 전파.

---

## 17. Hot-swap Action (무중단 교체)

DynamicPipeline 전용. 절차:

```
1. 해당 Action의 상태를 Sealing으로 전환
   → upstream의 output channel 잠금 (새 아이템 수신 중단)
   → 기존 워커들은 현재 큐 소진 후 종료 대기

2. 기존 워커 drain 완료 대기 (co_await)
   → 타임아웃 설정 가능: hot_swap_timeout (기본 30s)

3. 새 Action 설치
   → 새 워커 코루틴 spawn (Dispatcher::spawn_on 사용)

4. Sealing 해제 → Running으로 전환
   → upstream output channel 다시 연결
```

```cpp
// API
Task<Result<void>> DynamicPipeline::hot_swap(
    string_view name,
    shared_ptr<IDynamicAction> new_action,
    Duration timeout = 30s
);
```

**hot_swap 실패 조건**:
- `timeout` 내에 기존 워커 drain 완료 못함 → `errc::timed_out`
- 새 Action의 스키마가 인접 Action과 호환 안됨 → `errc::invalid_argument`
- Pipeline이 `Running` 상태가 아님 → `errc::operation_not_permitted`

---

## 18. 우선순위 채널

```cpp
template<typename T>
class PriorityChannel {
public:
    enum class Priority : uint8_t { High = 0, Normal = 1, Low = 2 };

    Task<Result<void>> send(T item, Priority p = Priority::Normal);
    Task<Result<T>>    recv();  // High 소진 → Normal 소진 → Low

    // Aging: Low가 N회 연속 스킵되면 다음 recv에서 강제 처리
    void set_aging_threshold(size_t n);  // 기본 100
};
```

**사용 사례**:
- High: 긴급 알람, 결제 트랜잭션
- Normal: 일반 API 요청
- Low: 배치 처리, 통계 집계

---

## 19. Config-driven Pipeline (설정 주도)

JSON/YAML에서 DynamicPipeline을 생성하는 팩토리.

```json
{
  "name": "order-processing",
  "type": "dynamic",
  "actions": [
    { "name": "validate",  "plugin": "libvalidate.so", "workers": 4 },
    { "name": "enrich",    "plugin": "libenrich.so",   "workers": 8, "auto_scale": true },
    { "name": "persist",   "plugin": "libpersist.so",  "workers": 2,
      "retry": { "max_attempts": 3, "backoff": "exponential" } }
  ],
  "triggers": [
    { "on_complete": "notification-pipeline" }
  ]
}
```

```cpp
class PipelineFactory {
public:
    // 플러그인 Action 등록 (.so 로드 없이 코드에서 직접 등록도 가능)
    void register_plugin(string_view name, function<shared_ptr<IDynamicAction>()> factory);

    Result<shared_ptr<DynamicPipeline>> from_json(string_view json);
    Result<shared_ptr<DynamicPipeline>> from_yaml(string_view yaml);
    Result<shared_ptr<PipelineGraph>>   graph_from_json(string_view json);
};
```

---

## 20. Pipeline Composition (파이프라인 합성)

StaticPipeline을 하나의 Action처럼 다른 Pipeline에 내장 가능.

```cpp
// SubpipelineAction: Pipeline<Mid, Out>을 Action<Mid, Out>처럼 래핑
template<typename In, typename Out>
class SubpipelineAction : public Action<In, Out> {
public:
    explicit SubpipelineAction(StaticPipeline<In, Out> inner);
};

// 사용 예시
auto inner = PipelineBuilder<Mid>{}
    .add(step_a)
    .add(step_b)
    .build();

auto outer = PipelineBuilder<In>{}
    .add(pre_action)
    .add(SubpipelineAction{std::move(inner)})  // 내부 파이프라인 삽입
    .add(post_action)
    .build();
```

**용도**: 공통 처리 로직의 재사용, 테스트 용이성 (inner pipeline을 mock으로 교체).

---

## 21. Arena 통합 (제로카피 아이템 할당)

기존 `Arena` / `FixedPoolResource` (arena.hpp)를 파이프라인 아이템 할당에 활용.

```cpp
// 각 reactor-local arena에서 파이프라인 아이템 래퍼 할당
// FixedPoolResource<sizeof(PipelineItem<T>)> 를 reactor당 하나 생성

template<typename T>
struct PipelineItem {
    T             value;
    TraceContext  trace_ctx;  // 전략 A 사용 시
    // 소유: FixedPoolResource 슬롯 (같은 reactor의 arena)
};

// AsyncChannel<T> 대신 ArenaChannel<T>:
// 아이템을 heap이 아닌 reactor-local pool에서 할당
// → malloc/free 없음, 캐시 효율 극대화
```

**적합한 상황**: 초고성능 경로, 아이템 크기가 고정적인 경우.
**주의**: Arena는 스레드 비안전 → 동일 reactor 내에서만 사용. Cross-reactor 전달 시 이동(move) 필요.

---

## 22. 관찰 가능성 — 메트릭

### 22.1 ActionMetrics

```cpp
struct ActionMetrics {
    atomic<uint64_t> items_processed{0};
    atomic<uint64_t> items_errored{0};
    atomic<uint64_t> items_retried{0};
    atomic<uint64_t> items_dlq{0};

    // 레이턴시 히스토그램 (< 1ms, < 10ms, < 100ms, >= 100ms)
    array<atomic<uint64_t>, 4> latency_buckets{};

    atomic<uint32_t> current_workers{0};
    atomic<uint32_t> circuit_breaker_opens{0};
    // channel depth는 channel.size()로 직접 조회
};
```

### 22.2 PipelineObserver 훅

```cpp
struct PipelineObserver {
    virtual void on_item_start (string_view action) {}
    virtual void on_item_done  (string_view action, Duration elapsed, bool ok) {}
    virtual void on_error      (string_view action, error_code ec) {}
    virtual void on_scale_event(string_view action, size_t old_n, size_t new_n) {}
    virtual void on_state_change(PipelineState from, PipelineState to) {}
    virtual void on_dlq_item   (string_view action) {}
    virtual void on_circuit_open  (string_view action) {}
    virtual void on_circuit_close (string_view action) {}
};

// 구현체
class PrometheusObserver : public PipelineObserver { ... };
class LoggingObserver    : public PipelineObserver { ... };
```

---

## 23. 파일 구조

```
include/qbuem/
│
├── core/
│   ├── reactor.hpp          ← post(fn) 추가
│   ├── dispatcher.hpp       ← post(), post_to(), spawn(), spawn_on() 추가
│   ├── epoll_reactor.hpp    ← post() 구현 (eventfd)
│   ├── io_uring_reactor.hpp ← post() 구현 (MSG_RING / eventfd 폴백)
│   └── kqueue_reactor.hpp   ← post() 구현 (EVFILT_USER)
│
└── pipeline/
    ├── channel.hpp          AsyncChannel<T>, PriorityChannel<T>
    ├── stream.hpp           Stream<T>, StreamItem<T>
    ├── action.hpp           Action<In,Out>, IDynamicAction, BatchAction<In,Out>
    ├── pipeline.hpp         StaticPipeline<In,Out>, DynamicPipeline, PipelineBuilder
    ├── pipeline_input.hpp   IPipelineInput<T>
    ├── pipeline_graph.hpp   PipelineGraph, PipelineFactory
    ├── message_bus.hpp      MessageBus, RequestEnvelope, xStreamEnvelope
    ├── tracing.hpp          TraceContext, Sampler, SpanExporter, PipelineTracer
    ├── resilience.hpp       RetryPolicy, CircuitBreaker, DeadLetter
    ├── metrics.hpp          ActionMetrics, PipelineMetrics, PipelineObserver
    ├── hot_swap.hpp         hot_swap 로직 (DynamicPipeline 내부)
    ├── config.hpp           PipelineFactory (JSON/YAML 파싱)
    ├── composition.hpp      SubpipelineAction
    └── generator.hpp        Generator<T>

src/pipeline/
    ├── channel.cpp
    ├── action.cpp
    ├── pipeline.cpp
    ├── pipeline_graph.cpp
    ├── message_bus.cpp
    ├── tracing.cpp
    └── config.cpp
```

---

## 24. 구현 순서 (의존성 그래프)

```
[0a] Reactor::post() 인터페이스
[0b] EpollReactor::post()       (eventfd)
[0c] IOUringReactor::post()     (MSG_RING 또는 eventfd)
[0d] KqueueReactor::post()      (EVFILT_USER)
[0e] Dispatcher::post() / spawn()
                │
                ▼
[1]  AsyncChannel<T>             ← [0e]
     PriorityChannel<T>          ← [1]
                │
         ┌──────┴──────┐
         ▼             ▼
[2a] Stream<T>      [2b] RetryPolicy / CircuitBreaker / DeadLetter (헤더 only)
         │             │
         └──────┬──────┘
                ▼
[3]  Action<In,Out>  IDynamicAction  BatchAction     ← [1][2a][2b]
                │
                ▼
[4a] StaticPipeline + PipelineBuilder                ← [3]
[4b] DynamicPipeline + hot_swap                      ← [3]
[4c] PipelineObserver / Metrics                      ← [4a][4b] 와 병행
                │
         ┌──────┴──────┐
         ▼             ▼
[5a] PipelineGraph    [5b] MessageBus                ← [4a][4b]
[5c] PipelineFactory  (Config-driven)                ← [5a][5b]
                │
                ▼
[6]  PipelineTracer + SpanExporter                   ← [4a][4b] 완료 후
[7]  SubpipelineAction (Composition)                  ← [4a]
[8]  통합 테스트 + 예제
```

**최소 MVP 경로**: `[0a→0b] → [1] → [3] → [4a]`
Linux 단일 플랫폼, StaticPipeline 기준. 나머지는 MVP 이후 단계적 추가.

**2단계 (운영 준비)**: `[4b] + [5a][5b] + [6]` (DynamicPipeline + Graph + Tracing)
**3단계 (고도화)**: `[5c] + [7] + PriorityChannel + Arena통합`

---

## 25. 부록: 기술 결정 요약

| 결정 사항 | 채택 | 이유 |
|-----------|------|------|
| 채널 알고리즘 | Dmitry Vyukov MPMC | ABA-free, cache-friendly, 검증됨 |
| EOS 표현 | `variant<T, StreamEnd>` | move-only 타입 완전 지원 |
| Scale-in 방식 | atomic target_workers + index 비교 | poison pill보다 예측 가능 |
| 취소 | `std::stop_token` | C++20 표준, 구조적 취소 |
| 에러 전파 | `Task<Result<Out>>` | unhandled_exception=terminate 대응 |
| Request-reply | Envelope + per-request reply channel | 동시 요청 demux |
| Cross-reactor wakeup | eventfd/EVFILT_USER + post() | 플랫폼별 native |
| Fan-out 타입 소거 | `IPipelineInput<T>` | heterogeneous graph |
| 정적 파이프라인 | `PipelineBuilder<In>.add<Out>().build()` | 컴파일타임 타입 체인 |
| 동적 파이프라인 | `IDynamicAction` + `ActionSchema` | 런타임 스키마 호환성 |
| Hot-swap | Seal → Drain → Swap → Resume | in-flight 유실 없음 |
| 트레이싱 전파 | thread-local TraceContext (전략 B) | 타입 오염 없음, 기존 패턴 일관 |
| 트레이싱 샘플링 | Pluggable Sampler 인터페이스 | 환경별 전략 교체 가능 |
| Arena 통합 | reactor-local FixedPoolResource | 기존 Arena 재사용, zero malloc |
| 우선순위 채널 | 3레벨 + aging | 스타베이션 방지 |
| Config 주도 | JSON/YAML → PipelineFactory | 코드 변경 없이 파이프라인 변경 |
| 파이프라인 합성 | SubpipelineAction 래퍼 | 재사용성, 테스트 용이성 |
