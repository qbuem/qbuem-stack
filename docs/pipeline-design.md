# qbuem Pipeline System — 기술 설계 문서

> **버전**: 0.3.0-draft
> **대상 브랜치**: `claude/continue-work-p2NeA`
> **검토 관점**: 분산 시스템 / C++20 코루틴 / Lock-free / 시스템 설계 / Observability / 컴파일러 최적화 / 의존성 관리 / 상태 격리

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
26. [State Management 개요 — 5가지 상태 스코프](#26-state-management-개요--5가지-상태-스코프)
27. [⚠️ 코루틴과 thread_local의 위험성](#27-️-코루틴과-thread_local의-위험성)
28. [Context 시스템 (아이템 스코프)](#28-context-시스템-아이템-스코프)
29. [ActionEnv — Action 호출 환경](#29-actionenv--action-호출-환경)
30. [ServiceRegistry — 스코프 기반 의존성 주입](#30-serviceregistry--스코프-기반-의존성-주입)
31. [Action 상태 분류 (의존성 강도)](#31-action-상태-분류-의존성-강도)
32. [Pipeline 간 의존성 강도](#32-pipeline-간-의존성-강도)
33. [Worker-local 상태 (락 없는 per-worker 패턴)](#33-worker-local-상태-락-없는-per-worker-패턴)
34. [상태 수명 및 소유권 규칙](#34-상태-수명-및-소유권-규칙)

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
| R11 | **상태 스코프 분리** | Global / Pipeline / Action / Item / Worker 5계층 |
| R12 | **Context 전파** | 아이템과 함께 이동하는 불변 key-value 컨텍스트 |
| R13 | **스코프 기반 의존성 주입** | ServiceRegistry 강/약 의존성, 계층적 스코프 |

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
// ActionEnv — Action 호출 환경 (§29 참조)
struct ActionEnv {
    Context         ctx;         // 아이템 스코프 불변 컨텍스트 (TraceCtx, RequestId, ...)
    std::stop_token stop;        // 파이프라인 drain/stop 취소 신호
    size_t          worker_idx;  // WorkerLocal<T>::get(idx) 접근용
};

template<typename In, typename Out>
class Action {
public:
    // 처리 함수: Task<Result<Out>> 반환 필수 (exception 금지 — unhandled_exception=terminate)
    // ActionEnv로 Context + stop_token + worker_idx 수신
    using Fn = std::function<Task<Result<Out>>(In, ActionEnv)>;

    struct Config {
        size_t         min_workers     = 1;
        size_t         max_workers     = 16;
        size_t         channel_cap     = 256;
        bool           auto_scale      = false;
        bool           keyed_ordering  = false;   // per-key consistent hash 배정
        RetryPolicy    retry           = RetryPolicy::none();
        CircuitBreaker circuit_breaker = {};
        ServiceRegistry* registry      = nullptr; // 파이프라인 스코프 레지스트리
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
    while (true) {
        if (my_idx >= ctx.target_workers.load(acquire)) co_return; // scale-in

        auto item_r = co_await ctx.in->recv();
        if (!item_r) co_return;  // channel closed (EOS)

        // 아이템 컨텍스트 구성: 업스트림 Context + 트레이싱 span 추가
        ActionEnv env{
            .ctx        = item_r->ctx,        // upstream Context 전파
            .stop       = ctx.stop_src.get_token(),
            .worker_idx = my_idx,
        };
        auto span = ctx.tracer.start_span(env.ctx);  // child span
        env.ctx = env.ctx.put(ActiveSpan{span});      // span을 Context에 추가

        auto out_r = co_await ctx.retry.execute(ctx.fn, item_r->value, env);

        ctx.tracer.end_span(span, out_r.has_value() ? std::error_code{} : out_r.error());

        if (!out_r) {
            co_await ctx.dlq->send(DeadLetter{item_r->value, out_r.error(), env.ctx});
            ctx.metrics.errors.fetch_add(1, relaxed);
            continue;
        }
        // 출력 아이템에 Context 전파 (downstream에서 같은 trace context 사용)
        co_await ctx.out->send(ContextualItem{std::move(*out_r), env.ctx});
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

> **§27 경고 먼저 확인**: 코루틴에서 `thread_local`은 co_await 경계를 넘으면 다른 스레드로
> 이동할 수 있어 **데이터 레이스**를 유발합니다. 파이프라인 아이템 스코프 상태에
> `thread_local`을 **사용하면 안 됩니다**.

파이프라인 아이템이 trace context를 운반하는 세 가지 전략:

**전략 A: 아이템 래핑 (Traced\<T\>) — 타입 오염**
```cpp
template<typename T>
struct Traced { T payload; TraceContext ctx; };
// Pipeline<Traced<MyType>, Traced<MyResult>>
// 단점: 모든 Action 타입이 바뀜, 기존 Action 재사용 불가
```

**전략 B: thread-local — ❌ 코루틴에서 위험**
```cpp
thread_local TraceContext current_trace_ctx;
// FATAL: co_await 후 다른 reactor 스레드에서 재개되면 wrong context 참조
```

**전략 C: Context 경유 ActionEnv — ✅ 권장**
```cpp
// TraceContext를 Context에 내장, ActionEnv.ctx로 전달 (§28, §29 참조)
struct ActiveSpan { SpanHandle span; };  // Context 슬롯

// Action에서 trace context 접근:
Task<Result<Out>> my_action(In item, ActionEnv env) {
    auto span = env.ctx.get<ActiveSpan>();  // 현재 span
    auto trace = env.ctx.get<TraceCtx>();   // trace_id / span_id
    // ...
}
// 장점: 타입 오염 없음, 코루틴에서 완전 안전, zero-overhead when disabled
```

**전략 C가 올바른 이유**: Context는 코루틴 프레임(ActionEnv)에 담겨 co_await를 넘어도
항상 올바른 값을 참조합니다. Reactor::current()처럼 infrastructure thread-local은
OK이지만, 아이템별로 달라지는 값에는 절대 thread-local을 쓰면 안 됩니다.

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
| 트레이싱 전파 | Context 슬롯 (ActionEnv.ctx) | thread_local은 코루틴에서 데이터 레이스 |
| 트레이싱 샘플링 | Pluggable Sampler 인터페이스 | 환경별 전략 교체 가능 |
| Arena 통합 | reactor-local FixedPoolResource | 기존 Arena 재사용, zero malloc |
| 우선순위 채널 | 3레벨 + aging | 스타베이션 방지 |
| Config 주도 | JSON/YAML → PipelineFactory | 코드 변경 없이 파이프라인 변경 |
| 파이프라인 합성 | SubpipelineAction 래퍼 | 재사용성, 테스트 용이성 |
| 상태 스코프 | 5계층 (Global/Pipeline/Action/Item/Worker) | 격리 + 공유 명확히 분리 |
| 아이템 컨텍스트 | 불변 persistent linked-list Context | O(1) copy, O(1) with(), 코루틴 안전 |
| Action 호출 환경 | ActionEnv{ctx, stop, worker_idx} | 모든 per-call 정보 단일 구조체 |
| 의존성 주입 | ServiceRegistry 계층 (Global → Pipeline) | 강/약 의존성 명시, 테스트 가능 |
| Worker 로컬 상태 | WorkerLocal<T> (vector<T> + idx) | 락 없는 per-worker 상태 |

---

## 26. State Management 개요 — 5가지 상태 스코프

파이프라인 상태를 **어디에 두느냐**가 성능, 안전성, 테스트 가능성을 결정합니다.

```
┌────────────────────────────────────────────────────────────────────────┐
│ Scope 1: Global State                                                  │
│   수명: 프로세스 전체   공유: 모든 파이프라인 / 모든 Action             │
│   예: DB 커넥션 풀, 설정(Config), Metrics sink, Logger                 │
│   접근: GlobalRegistry::get<T>()                                       │
│                                                                        │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │ Scope 2: Pipeline State                                         │   │
│  │   수명: 파이프라인 Running 구간   공유: 같은 파이프라인 내 모든 Action │
│  │   예: CircuitBreaker 상태, 파이프라인 레벨 Rate Limiter, 파이프라인 캐시│
│  │   접근: PipelineRegistry::get<T>() (GlobalRegistry 상속)        │   │
│  │                                                                 │   │
│  │  ┌──────────────────────────────────────────────────────────┐  │   │
│  │  │ Scope 3: Action State                                    │  │   │
│  │  │   수명: Action 객체 수명 (파이프라인 수명 이내)           │  │   │
│  │  │   공유: 해당 Action의 모든 워커 (스레드 안전 필요)        │  │   │
│  │  │   예: ML 모델 가중치, 컴파일된 정규식, 연결 풀            │  │   │
│  │  │   접근: Action 생성자 주입 (ServiceRegistry 경유)         │  │   │
│  │  │                                                          │  │   │
│  │  │  ┌───────────────────────────────────────────────────┐  │  │   │
│  │  │  │ Scope 4: Item State (Context)                     │  │  │   │
│  │  │  │   수명: 단일 아이템이 파이프라인을 통과하는 동안   │  │  │   │
│  │  │  │   공유: 없음 (아이템 전용, 불변)                   │  │  │   │
│  │  │  │   예: TraceContext, RequestId, AuthClaims, Deadline│  │  │   │
│  │  │  │   접근: ActionEnv.ctx.get<Slot>()                 │  │  │   │
│  │  │  └───────────────────────────────────────────────────┘  │  │   │
│  │  │  ┌───────────────────────────────────────────────────┐  │  │   │
│  │  │  │ Scope 5: Worker State                             │  │  │   │
│  │  │  │   수명: 워커 코루틴 수명                           │  │  │   │
│  │  │  │   공유: 없음 (워커 전용, 락 불필요)                │  │  │   │
│  │  │  │   예: Per-worker RNG, 임시 버퍼, 작은 LRU 캐시    │  │  │   │
│  │  │  │   접근: WorkerLocal<T>::get(env.worker_idx)       │  │  │   │
│  │  │  └───────────────────────────────────────────────────┘  │  │   │
│  │  └──────────────────────────────────────────────────────────┘  │   │
│  └─────────────────────────────────────────────────────────────────┘   │
└────────────────────────────────────────────────────────────────────────┘
```

### 스코프별 정리표

| 스코프 | 수명 | 공유 범위 | 접근 방법 | 스레드 안전 요구 |
|--------|------|----------|----------|----------------|
| **Global** | 프로세스 | 모든 곳 | `GlobalRegistry::get<T>()` | 반드시 (읽기는 OK) |
| **Pipeline** | 파이프라인 | 같은 파이프라인 | `PipelineRegistry::get<T>()` | 반드시 |
| **Action** | Action 객체 | 같은 Action 워커들 | 생성자 주입 | 반드시 (가변 상태) |
| **Item** | 아이템 통과 시간 | 없음 (불변) | `ActionEnv.ctx.get<Slot>()` | 불필요 (불변) |
| **Worker** | 워커 코루틴 | 없음 | `WorkerLocal<T>::get(idx)` | 불필요 (전용) |

---

## 27. ⚠️ 코루틴과 thread_local의 위험성

이것은 qbuem-stack에서 가장 흔한 버그 원인입니다. **반드시 이해해야 합니다.**

### 문제: co_await 경계에서 스레드 교체

```
스레드 A:  [Action 시작] → thread_local = "user-123" → co_await channel.recv()
                                                              │
                                                     [SUSPENDED — 스레드 반납]
                                                              │
스레드 B:                              [다른 작업 처리] → [위 코루틴 resume]
                                                              │
스레드 B에서 재개됨! thread_local은 여전히 스레드 B의 값을 가리킴 → WRONG!
```

```cpp
// ❌ WRONG: thread_local은 코루틴 아이템 상태에 절대 사용 불가
thread_local std::string current_request_id;

Task<Result<Out>> process(In item, ActionEnv env) {
    current_request_id = "req-123";      // 스레드 A에 설정
    auto r = co_await some_async_op();   // ← 여기서 스레드 B로 이동할 수 있음
    log(current_request_id);             // ← 스레드 B의 thread_local! 다른 값
    co_return ...;
}

// ✅ CORRECT: Context는 코루틴 프레임 내에 있어 항상 올바른 값
Task<Result<Out>> process(In item, ActionEnv env) {
    auto req_id = env.ctx.get<RequestId>();  // 코루틴 프레임 내부 (안전)
    auto r = co_await some_async_op();       // 스레드 교체 후에도
    log(req_id->value);                      // 항상 올바른 값 (코루틴 프레임에 있음)
    co_return ...;
}
```

### thread_local이 OK한 경우 (인프라 전용)

```
✅ Reactor::current()              — reactor 스레드 시작 시 설정, 변경 없음
✅ Arena::thread_local_arena()     — reactor 스레드 전용 할당자, 변경 없음
❌ current_trace_context           — 아이템별로 다름 → Context 슬롯 사용
❌ current_request_id              — 아이템별로 다름 → Context 슬롯 사용
❌ current_user                    — 아이템별로 다름 → Context 슬롯 사용
```

**규칙**: `thread_local`은 reactor 스레드의 수명 동안 값이 고정된 인프라 객체에만 사용.
아이템별로 다른 값은 반드시 `Context` 슬롯을 사용.

---

## 28. Context 시스템 (아이템 스코프)

### 28.1 설계 요구사항

- **불변성**: `ctx.put(slot, val)` 은 새 Context를 반환, 기존 ctx 미수정
- **저렴한 복사**: `shared_ptr` 복사 = O(1) (deep copy 없음)
- **코루틴 안전**: 코루틴 프레임에 담겨 스레드 교체와 무관
- **타입 안전**: 슬롯 타입 = 저장 타입 (type_index 기반, 캐스트 없음)
- **Fork 시맨틱**: fan-out 시 자식들이 부모 Context를 독립적으로 확장 가능

### 28.2 구현

```cpp
// 슬롯 타입 = Context에 저장할 값의 타입 (전체가 key+value)
// 예: struct RequestId { std::string value; };
//     ctx.put(RequestId{"req-123"}) / ctx.get<RequestId>()

class Context {
    // Persistent 단방향 linked list: O(1) prepend, O(1) shared copy
    // put() = 새 노드를 head에 prepend한 새 Context (원본 불변)
    struct Node {
        std::type_index           type_key;
        std::shared_ptr<void>     value;
        std::shared_ptr<const Node> next;
    };
    std::shared_ptr<const Node> head_;

public:
    static const Context empty;

    // 슬롯 추가 (기존 타입이 있으면 shadow — 원본은 여전히 접근 가능)
    template<typename T>
    [[nodiscard]] Context put(T value) const {
        auto node = std::make_shared<Node>(Node{
            .type_key = std::type_index(typeid(T)),
            .value    = std::make_shared<T>(std::move(value)),
            .next     = head_,
        });
        Context c;
        c.head_ = std::move(node);
        return c;
    }

    // 슬롯 조회 (없으면 nullopt, 존재하면 T의 복사본)
    template<typename T>
    [[nodiscard]] std::optional<T> get() const noexcept {
        auto tid = std::type_index(typeid(T));
        for (const Node* n = head_.get(); n; n = n->next.get()) {
            if (n->type_key == tid)
                return *std::static_pointer_cast<T>(n->value);
        }
        return std::nullopt;
    }

    // 슬롯 포인터 조회 (복사 없이 참조, nullptr = 없음)
    template<typename T>
    [[nodiscard]] const T* get_ptr() const noexcept {
        auto tid = std::type_index(typeid(T));
        for (const Node* n = head_.get(); n; n = n->next.get()) {
            if (n->type_key == tid)
                return std::static_pointer_cast<T>(n->value).get();
        }
        return nullptr;
    }

    bool empty() const noexcept { return !head_; }
};
```

### 28.3 내장 Context 슬롯

```cpp
// 파이프라인 인프라가 자동으로 관리하는 슬롯들
// Action에서는 읽기만 (put으로 shadow 가능하지만 권장하지 않음)

struct TraceCtx {
    uint8_t  trace_id[16];  // W3C trace_id (128-bit)
    uint8_t  span_id[8];    // 현재 span (64-bit)
    uint8_t  trace_flags;   // sampled bit
};

struct RequestId    { std::string value; };
struct AuthSubject  { std::string_view value; };   // 인증된 사용자 식별자
struct AuthRoles    { std::vector<std::string_view> value; };
struct Deadline     { std::chrono::steady_clock::time_point value; };
struct ActiveSpan   { SpanHandle span; };          // 현재 tracing span

// HTTP → Pipeline 진입점에서 자동 설정:
// Context ctx = Context::empty
//     .put(TraceCtx::from_traceparent(req.header("traceparent")))
//     .put(RequestId{req.header("X-Request-Id")})
//     .put(AuthSubject{res.get_header("X-Auth-Sub")});
// co_await pipeline.push(ContextualItem{req, ctx});
```

### 28.4 Fork 시맨틱 (fan-out 시 컨텍스트 분기)

```cpp
// fan-out: 하나의 Context → 여러 자식 Context (각자 독립 확장)
auto parent_ctx = env.ctx;

// 각 fan-out 대상이 서로 다른 span을 가지도록
auto ctx_a = parent_ctx.put(ActiveSpan{tracer.start_span("branch-a", parent_ctx)});
auto ctx_b = parent_ctx.put(ActiveSpan{tracer.start_span("branch-b", parent_ctx)});

co_await pipeline_a.push(ContextualItem{item, ctx_a});
co_await pipeline_b.push(ContextualItem{item, ctx_b});
// parent_ctx는 변경되지 않음 (불변)
```

### 28.5 ContextualItem — 채널 전송 단위

```cpp
// AsyncChannel이 실제로 전송하는 타입 (아이템 + 컨텍스트 묶음)
template<typename T>
struct ContextualItem {
    T       value;
    Context ctx;
};

// Action의 입출력은 ContextualItem<T> 가 아닌 T 로 보임
// (파이프라인 인프라가 ContextualItem 래핑/언래핑 처리)
// → Action Fn의 서명: Task<Result<Out>>(In item, ActionEnv env)
//   파이프라인이 channel에서 ContextualItem<In>을 꺼내 {.value, ActionEnv{.ctx}} 분리
```

---

## 29. ActionEnv — Action 호출 환경

Action 처리 함수가 매 호출마다 받는 **per-call 환경 구조체**.
생성 비용: `shared_ptr` 복사(ctx) + 원시값 2개 = ~16 bytes stack.

```cpp
struct ActionEnv {
    Context         ctx;         // 아이템 스코프 불변 컨텍스트
    std::stop_token stop;        // 파이프라인 drain/stop 취소 신호
    size_t          worker_idx;  // WorkerLocal<T>::get(idx) 접근용
};

// Action 처리 함수 서명 (예외 금지 — unhandled_exception=terminate)
// Task<Result<Out>>(In item, ActionEnv env)
```

### 29.1 ActionEnv 활용 패턴

```cpp
// ① Context에서 요청 메타데이터 읽기
Task<Result<Out>> enrich_action(In item, ActionEnv env) {
    auto req_id = env.ctx.get<RequestId>();
    auto subject = env.ctx.get<AuthSubject>();

    if (!subject) co_return std::unexpected(errc::permission_denied);

    // ② stop_token으로 취소 체크 (co_await 전 반드시)
    if (env.stop.stop_requested())
        co_return std::unexpected(errc::operation_canceled);

    // ③ WorkerLocal로 락 없는 per-worker 상태 접근
    auto& rng = rngs_.get(env.worker_idx);
    auto noise = std::uniform_real_distribution{}(rng);

    // ④ 자식 컨텍스트 생성 (다운스트림에 추가 정보 전달)
    // 주의: ActionEnv.ctx는 불변 — put()으로 새 ctx 생성
    // (파이프라인이 자동으로 다음 Action에 전파)

    co_return Out{item, noise};
}
```

### 29.2 컨텍스트 확장 — downstream에 새 정보 전달

```cpp
// Action이 downstream에 새 슬롯을 추가하고 싶을 때:
// → 처리 결과에 context 수정 의도를 담아 반환하는 방식은 복잡함
// → 권장: ContextualOut<T>를 Out 타입으로 사용

struct ContextualOut {
    Out     value;
    Context ctx_override;  // 비어있으면 upstream ctx 그대로 전파
};

// 파이프라인이 ContextualOut을 감지하면 ctx_override 사용, 없으면 부모 ctx 전파
```

---

## 30. ServiceRegistry — 스코프 기반 의존성 주입

### 30.1 설계 원칙

- **Action의 외부 의존성은 생성자에서 주입** (call-time이 아님)
- **ServiceRegistry는 파이프라인 구성 시 한 번만 읽음** (runtime read-mostly)
- **계층적 스코프**: PipelineRegistry가 없으면 GlobalRegistry에서 fallback 조회
- **강한 의존성** (`require<T>`): 없으면 프로세스 시작 불가 → 빠른 실패(fail-fast)
- **약한 의존성** (`get<T>`): 없으면 `nullptr` → graceful degradation

```cpp
class ServiceRegistry {
public:
    // 싱글톤 등록: 이미 생성된 인스턴스
    template<typename T>
    void register_singleton(std::shared_ptr<T> instance) {
        services_[std::type_index(typeid(T))] = instance;
    }

    // 팩토리 등록: 첫 조회 시 lazy 생성 (lazy singleton)
    template<typename T>
    void register_factory(std::function<std::shared_ptr<T>()> factory) {
        factories_[std::type_index(typeid(T))] = [factory = std::move(factory)]() {
            return std::static_pointer_cast<void>(factory());
        };
    }

    // 약한 의존성 조회: 없으면 nullptr (graceful degradation)
    template<typename T>
    std::shared_ptr<T> get() const {
        auto key = std::type_index(typeid(T));
        if (auto it = services_.find(key); it != services_.end())
            return std::static_pointer_cast<T>(it->second);
        if (auto it = factories_.find(key); it != factories_.end()) {
            auto v = it->second();
            services_[key] = v;  // cache
            return std::static_pointer_cast<T>(v);
        }
        if (parent_) return parent_->get<T>();
        return nullptr;
    }

    // 강한 의존성 조회: 없으면 terminate() (fail-fast)
    template<typename T>
    std::shared_ptr<T> require() const {
        auto s = get<T>();
        if (!s) {
            // 에러 메시지 출력 후 종료 (디버깅 용이)
            // "[qbuem] FATAL: required service not registered: T"
            std::terminate();
        }
        return s;
    }

    ServiceRegistry* parent_ = nullptr;

private:
    mutable std::unordered_map<std::type_index, std::shared_ptr<void>> services_;
    mutable std::unordered_map<std::type_index, std::function<std::shared_ptr<void>()>> factories_;
};

// 전역 레지스트리 (프로세스 싱글톤)
ServiceRegistry& global_registry();

// 파이프라인 스코프 레지스트리 (전역 상속 + 오버라이드)
// 생성: PipelineRegistry pr{&global_registry()};
// 파이프라인별 CB 상태, Rate Limiter 등록
using PipelineRegistry = ServiceRegistry;  // parent_ 연결로 계층 구성
```

### 30.2 Action에서의 의존성 주입 패턴

```cpp
// Action은 생성자에서 ServiceRegistry를 받아 의존성 해결
// → call-time(ActionEnv)이 아닌 구성 시간에 해결 → 런타임 오버헤드 없음

class EnrichAction {
    std::shared_ptr<IDatabase>   db_;     // 강한 의존성: 없으면 동작 불가
    std::shared_ptr<ICache>      cache_;  // 약한 의존성: 없어도 DB로 폴백

public:
    explicit EnrichAction(ServiceRegistry& registry)
        : db_   (registry.require<IDatabase>())
        , cache_(registry.get<ICache>())          // nullptr OK
    {}

    Task<Result<EnrichedItem>> process(RawItem item, ActionEnv env) {
        if (env.stop.stop_requested())
            co_return std::unexpected(errc::operation_canceled);

        // 약한 의존성: cache가 있으면 먼저 시도
        if (cache_) {
            if (auto cached = co_await cache_->get(item.id))
                co_return EnrichedItem{item, *cached};
        }

        // 강한 의존성: DB 조회 (항상 있음)
        auto result = co_await db_->query(item.id);
        if (!result) co_return std::unexpected(result.error());

        if (cache_)
            co_await cache_->set(item.id, *result);

        co_return EnrichedItem{item, *result};
    }

private:
    WorkerLocal<std::mt19937> rngs_;  // worker-local 상태 (§33)
};

// Action 생성:
ServiceRegistry pipeline_reg{&global_registry()};
pipeline_reg.register_singleton<ICache>(make_shared<RedisCache>());

auto action = Action<RawItem, EnrichedItem>{
    [ea = EnrichAction{pipeline_reg}](RawItem item, ActionEnv env) mutable {
        return ea.process(std::move(item), env);
    }
};
```

### 30.3 강/약 의존성 결정 기준

| 기준 | 강한 의존성 (`require`) | 약한 의존성 (`get`) |
|------|------------------------|---------------------|
| 없으면? | Action이 동작 불가 | graceful degradation 가능 |
| 예시 | DB 커넥션 풀, Auth 서비스 | Cache, 외부 알림 서비스 |
| 실패 시 | 빠른 실패 (시작 시 확인) | nullptr 체크 후 폴백 |
| 테스트 | mock 주입 필수 | mock 선택적 |

---

## 31. Action 상태 분류 (의존성 강도)

### 분류 1: Stateless Action (순수 함수)

```cpp
// 내부 상태 없음 → 이상적 기본 형태
// 멱등성, 완전한 테스트 용이성, 병렬화 완전 안전
auto validate_action = Action<RawItem, ValidatedItem>{
    [](RawItem item, ActionEnv env) -> Task<Result<ValidatedItem>> {
        if (item.id.empty())
            co_return std::unexpected(errc::invalid_argument);
        co_return ValidatedItem{item};
    }
};
```

### 분류 2: Immutable-Stateful Action (읽기 전용 상태)

```cpp
// 초기화 후 읽기만 → 공유 가능, 락 불필요
class PatternAction {
    std::regex pattern_;  // 컴파일 시 고정, 이후 읽기만

public:
    explicit PatternAction(std::string_view regex_str)
        : pattern_(regex_str.data()) {}

    Task<Result<MatchedItem>> process(TextItem item, ActionEnv env) {
        // pattern_은 읽기 전용 → 스레드 안전
        if (!std::regex_search(item.text, pattern_))
            co_return std::unexpected(errc::no_match);
        co_return MatchedItem{item};
    }
};
```

### 분류 3: Mutable-Stateful Action — 두 가지 선택지

#### 옵션 A: 스레드 안전 (여러 워커가 공유)

```cpp
class CounterAction {
    std::atomic<uint64_t> count_{0};  // atomic → 스레드 안전

public:
    Task<Result<Out>> process(In item, ActionEnv env) {
        auto n = count_.fetch_add(1, std::memory_order_relaxed);
        co_return Out{item, n};
    }
};
```

#### 옵션 B: WorkerLocal (§33) — 락 완전 제거

```cpp
class CachingAction {
    WorkerLocal<LruCache<Key, Value>> caches_;  // 워커별 독립 캐시

public:
    explicit CachingAction(size_t worker_count)
        : caches_(worker_count, [] { return LruCache<Key, Value>{1024}; }) {}

    Task<Result<Out>> process(In item, ActionEnv env) {
        auto& cache = caches_.get(env.worker_idx);  // 락 없음
        if (auto hit = cache.get(item.key)) co_return Out{*hit};
        auto result = co_await fetch(item);
        cache.set(item.key, *result);
        co_return Out{*result};
    }
};
```

### 분류 4: External-Stateful Action (외부 시스템)

```cpp
// DB, Redis, Kafka 등 외부 상태 — ServiceRegistry로 주입
class PersistAction {
    std::shared_ptr<IDatabase> db_;  // 생성자 주입 (§30.2)

public:
    explicit PersistAction(ServiceRegistry& reg)
        : db_(reg.require<IDatabase>()) {}

    Task<Result<SavedItem>> process(ProcessedItem item, ActionEnv env) {
        // DB는 자체적으로 스레드 안전 (connection pool 내부 처리)
        auto result = co_await db_->save(item);
        if (!result) co_return std::unexpected(result.error());
        co_return SavedItem{*result};
    }
};
```

### 분류 요약

| 분류 | 공유 가능 | 락 필요 | 선호도 |
|------|----------|--------|--------|
| Stateless | ✅ 완전 | 없음 | ⭐⭐⭐ 최우선 |
| Immutable-Stateful | ✅ 읽기만 | 없음 | ⭐⭐⭐ |
| Mutable (atomic) | ✅ | atomic | ⭐⭐ |
| Mutable (WorkerLocal) | ❌ (워커별) | 없음 | ⭐⭐⭐ (성능 중요 시) |
| External | ✅ (pool) | pool 내부 | ⭐⭐ |

---

## 32. Pipeline 간 의존성 강도

### 강한 의존성 (Strong) — 타입 체인 자체

```
StaticPipeline에서의 강한 의존성:
  Action<HttpRequest, AuthedReq>
         ↓ 출력 타입 = 다음 Action의 입력 타입
  Action<AuthedReq, ParsedBody>    ← AuthedReq가 없으면 컴파일 에러
         ↓
  Action<ParsedBody, SavedItem>    ← ParsedBody가 없으면 컴파일 에러

타입 불일치 = 컴파일 에러 → 런타임 패닉 없음
```

**특징**:
- 선행 Action이 실패하면 후속 Action은 실행 안됨 (DLQ로 이동)
- Action을 제거하거나 타입을 변경하면 전체 파이프라인 재컴파일 필요
- 의도적 설계: 핵심 처리 경로의 정확성 보장

### 약한 의존성 (Weak) — ServiceRegistry + 옵셔널 접근

```
여러 파이프라인이 공통 서비스를 공유:
  Pipeline A ─── GlobalRegistry::get<ICache>() ──→ Redis Cache
  Pipeline B ─── GlobalRegistry::get<ICache>() ──┘  (같은 캐시 공유)

  Cache가 없어도 → nullptr 체크 후 DB 폴백 (graceful degradation)
```

**특징**:
- 서비스 부재 시 degraded mode로 동작 가능
- 런타임에 서비스 교체/업그레이드 가능 (DynamicPipeline hot-swap과 연계)
- 파이프라인 간 직접 참조 없음 → 느슨한 결합

### 파이프라인 간 의존성 그래프

```
PipelineGraph에서 세 가지 의존성 종류:

1. 완료 트리거 (순서 의존):
   Pipeline A → (완료 후) → Pipeline B
   A가 끝나야 B 시작, 하지만 타입은 독립적

2. 공유 채널 (데이터 의존):
   Pipeline A ──→ AsyncChannel<T> ──→ Pipeline B
   A의 출력이 B의 입력 (MessageBus 패턴)

3. 공유 서비스 (리소스 의존):
   Pipeline A ──→ GlobalRegistry::get<IDatabase>() ←── Pipeline B
   같은 DB를 독립적으로 사용 (직접 의존 없음)
```

### 파이프라인 간 순환 의존성 감지

```cpp
// PipelineGraph::start() 내부:
// Kahn's algorithm으로 위상 정렬
// 사이클 감지 시: Result::err(errc::invalid_argument)
//   + "cycle detected: A → B → C → A" 에러 메시지
```

---

## 33. Worker-local 상태 (락 없는 per-worker 패턴)

여러 워커가 동일한 타입의 상태를 독립적으로 소유. 락 완전 제거.

```cpp
template<typename T>
class WorkerLocal {
    // 각 워커가 전용 슬롯 소유 → false sharing 방지를 위해 padding 고려
    struct alignas(64) Slot { T value; };  // cache-line 정렬
    std::vector<Slot> slots_;

public:
    // 워커 수 + 초기값 팩토리로 생성
    explicit WorkerLocal(size_t worker_count, std::function<T()> factory) {
        slots_.reserve(worker_count);
        for (size_t i = 0; i < worker_count; ++i)
            slots_.emplace_back(Slot{factory()});
    }

    // O(1) 접근, 락 없음 (워커만 자신의 슬롯 접근)
    T&       get(size_t worker_idx)       { return slots_[worker_idx].value; }
    const T& get(size_t worker_idx) const { return slots_[worker_idx].value; }
};
```

### 사용 사례

```cpp
class ImageProcessAction {
    // 워커별 전용 리소스 — 락 없이 안전하게 사용
    WorkerLocal<std::mt19937>           rngs_;     // per-worker RNG (시드 다름)
    WorkerLocal<std::vector<uint8_t>>   buffers_;  // per-worker 재사용 버퍼
    WorkerLocal<LruCache<Url, Image>>   caches_;   // per-worker 이미지 캐시

public:
    explicit ImageProcessAction(size_t worker_count)
        : rngs_   (worker_count, [] { return std::mt19937{std::random_device{}()}; })
        , buffers_(worker_count, [] { return std::vector<uint8_t>(4096); })
        , caches_ (worker_count, [] { return LruCache<Url, Image>{256}; })
    {}

    Task<Result<ProcessedImage>> process(ImageRequest req, ActionEnv env) {
        auto& rng    = rngs_.get(env.worker_idx);    // 락 없음
        auto& buffer = buffers_.get(env.worker_idx);  // 락 없음
        auto& cache  = caches_.get(env.worker_idx);   // 락 없음

        if (auto hit = cache.get(req.url)) co_return ProcessedImage{*hit};

        buffer.resize(req.size);
        auto noise = std::uniform_real_distribution{0.0, 0.1}(rng);
        // ... 처리 ...
        co_return ProcessedImage{/* ... */};
    }
};
```

### WorkerLocal vs thread_local

| | `WorkerLocal<T>` | `thread_local T` |
|--|-----------------|-----------------|
| 접근 방법 | `wl.get(env.worker_idx)` | 자동 |
| 코루틴 안전 | ✅ (idx는 프레임에 있음) | ❌ (co_await 후 다른 스레드) |
| 공유 제어 | ✅ (명시적 idx) | ❌ (자동, 추적 어려움) |
| 워커 수 변경 | ✅ (scale_to 연동) | ❌ |
| 테스트 가능 | ✅ | 어려움 |

---

## 34. 상태 수명 및 소유권 규칙

### 34.1 소유권 계층

```
전역 상태:
  GlobalRegistry (프로세스 싱글톤, static lifetime)
  → std::shared_ptr<T> (소유)
  → 파이프라인/Action이 shared_ptr 복사 (공동 소유)

파이프라인 상태:
  PipelineRegistry (파이프라인 lifetime)
  → CircuitBreaker, RateLimiter (파이프라인이 단독 소유)
  → GlobalRegistry fallback (공동 소유)

Action 상태:
  Action 객체 (파이프라인이 소유하는 unique_ptr<Action>)
  → WorkerLocal<T> (Action이 단독 소유)
  → ServiceRegistry에서 가져온 shared_ptr (공동 소유)

아이템 상태 (Context):
  Context (shared_ptr 기반 persistent list)
  → 채널을 통해 전달 (이동 시 소유권 이전)
  → fan-out 시 복사 (공동 소유, 불변이라 안전)
```

### 34.2 수명 규칙 요약

```cpp
// 1. GlobalRegistry에 등록된 서비스는 모든 파이프라인보다 오래 살아야 함
//    → main()에서 생성, 모든 파이프라인 stop() 후 소멸

// 2. PipelineRegistry는 해당 파이프라인과 수명 일치
//    → pipeline.start() 전 구성, pipeline 소멸 시 자동 소멸

// 3. Action이 ServiceRegistry에서 가져온 shared_ptr은 Action 수명 동안 유효
//    → Action 소멸 → shared_ptr 소멸 → 레퍼런스 카운트 감소

// 4. Context는 아이템 처리 완료 시 자동 해제
//    → 채널에서 꺼낼 때 move, DLQ로 갈 때도 move
//    → fan-out: 복사본들이 각자 해제 (shared_ptr 레퍼런스 카운트)

// 5. WorkerLocal<T>는 Action과 수명 일치
//    → Action 소멸 시 모든 worker 슬롯 자동 소멸
```

### 34.3 Pipeline 종료 시 순서

```
1. pipeline.drain()          — 새 아이템 수신 중단, in-flight 완료 대기
2. Context들 자동 해제       — drain 완료 시 마지막 아이템 Context 소멸
3. Action 소멸               — WorkerLocal, 주입된 서비스 shared_ptr 해제
4. PipelineRegistry 소멸     — Pipeline-scoped 서비스 해제
5. GlobalRegistry 마지막 해제 — 레퍼런스 카운트가 0이 되면 실제 소멸
```

### 34.4 흔한 수명 버그와 해결책

```cpp
// ❌ BUG: Action이 글로벌 객체를 raw pointer로 잡음
//         → 글로벌 먼저 소멸 시 dangling pointer
class BadAction {
    IDatabase* db_;  // raw pointer!
public:
    BadAction(IDatabase* db) : db_(db) {}
};

// ✅ FIX: shared_ptr으로 공동 소유
class GoodAction {
    std::shared_ptr<IDatabase> db_;  // 공동 소유 → 수명 보장
public:
    explicit GoodAction(ServiceRegistry& reg)
        : db_(reg.require<IDatabase>()) {}
};

// ❌ BUG: Context에 스택 변수 포인터 저장
//         → 함수 반환 후 dangling
Context bad_ctx = ctx.put(AuthSubject{
    .value = std::string_view{some_local_string}  // 스택 string → dangling!
});

// ✅ FIX: string을 Context에 복사로 저장, string_view는 Context 내부에만 사용
Context good_ctx = ctx.put(RequestId{std::string{req.header("X-Request-Id")}});
```
