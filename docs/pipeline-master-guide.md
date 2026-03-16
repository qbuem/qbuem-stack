# qbuem-stack Pipeline System: The Complete Guide

`qbuem-stack` Pipeline 시스템의 완전한 레퍼런스입니다.
핵심 아키텍처, 설계 패턴, IPC 연계, 실전 레시피를 모두 다룹니다.

---

## 1. Overview & Core Philosophy

Pipeline 시스템은 "Three Zeros" 철학을 따릅니다: **Zero Latency, Zero Allocation, Zero Dependency**.

### 핵심 원칙

- **Lock-Free by Default**: C++20 코루틴 + MPMC/SPSC ring buffer로 mutex 제거.
- **Worker Isolation (Bulkheading)**: 스테이지마다 독립 worker pool — 하나의 병목이 전체를 막지 않음.
- **Natural Backpressure**: 소비자 포화 시 `co_await`로 생산자를 언어 레벨에서 일시 정지.
- **Mechanical Sympathy**: NUMA-aware 스케줄링, cache-aligned 구조체.

---

## 2. System Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  Layer 2: StaticPipeline / DynamicPipeline / PipelineGraph  │
│   PipelineBuilder (with_source · add · with_sink · build)   │
├─────────────────────────────────────────────────────────────┤
│  Layer 1: Action<In, Out>                                   │
│   Worker Pool · ActionEnv · ServiceRegistry                 │
├─────────────────────────────────────────────────────────────┤
│  Layer 0: AsyncChannel<T>                                   │
│   Vyukov MPMC · cross-reactor wakeup via Reactor::post()    │
├─────────────────────────────────────────────────────────────┤
│  Foundation: Dispatcher · Reactor (epoll/kqueue/io_uring)   │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. Static vs. Dynamic Pipelines

### 3-1. StaticPipeline\<In, Out\>

컴파일 타임 고정 타입 체인. 성능이 최우선인 경로에 사용합니다.

```cpp
auto pipeline = PipelineBuilder<RawOrder, RawOrder>{}
    .add<ParsedOrder>(stage_parse)     // RawOrder  → ParsedOrder
    .add<ParsedOrder>(stage_enrich)    // ParsedOrder → ParsedOrder
    .add<ValidatedOrder>(stage_validate) // ParsedOrder → ValidatedOrder
    .build();   // StaticPipeline<RawOrder, ValidatedOrder>

pipeline.start(dispatcher);
co_await pipeline.push(RawOrder{...});
```

| 특성 | 값 |
| :--- | :--- |
| 타입 안전 | 컴파일 타임 완전 검증 |
| 오버헤드 | Zero (전체 인라이닝) |
| 런타임 변경 | 불가 |

### 3-2. DynamicPipeline\<T\>

런타임 타입 소거 (`std::any` + 가상 호출). Hot-swap이 필요할 때 사용합니다.

```cpp
DynamicPipeline<ValidatedOrder> pipeline;
pipeline.add_stage("risk_check", stage_risk_check);
pipeline.add_stage("record",     stage_record);
pipeline.start(dispatcher);

// 런타임 스테이지 교체
pipeline.hot_swap("risk_check", new_risk_check_v2);
```

### 선택 기준

| 상황 | 권장 |
| :--- | :--- |
| 컴파일 타임 타입 결정 | `StaticPipeline` |
| Config 파일 기반 구성 | `DynamicPipeline` |
| 런타임 로직 교체 필요 | `DynamicPipeline` + `hot_swap()` |
| 최고 성능 필요 | `StaticPipeline` |

---

## 4. IPC 연계: Three-Layer Messaging Architecture

qbuem-stack은 세 가지 메시징 레이어를 Pipeline과 완전히 연계합니다.

```
[SHMChannel]  ─── SHMSource ──→  Pipeline  ──→ SHMSink ──→  [SHMChannel]
[MessageBus]  ─── MessageBusSource ──→  Pipeline  ──→ MessageBusSink ──→  [MessageBus]
[SHMBus]      ─── subscribe() + push() ──→  Pipeline
```

### 4-1. PipelineBuilder::with_source()

외부 소스를 Pipeline Head에 연결합니다.

```cpp
// SourceT 요건: init() → Result<void>, next() → Task<optional<const T*>>
template <typename SourceT>
PipelineBuilder<OrigIn, CurOut> with_source(SourceT src, size_t cap = 256);
```

- `with_source()`는 `add()` 앞에 호출합니다.
- 내부적으로 `AsyncChannel`을 생성하고 소스 펌프 코루틴을 등록합니다.
- `init()`이 실패하면 펌프가 시작되지 않습니다 (무음 처리).

### 4-2. PipelineBuilder::with_sink()

외부 싱크를 Pipeline Tail에 연결합니다.

```cpp
// SinkT 요건: init() → Result<void>, sink(const T&) → Task<Result<void>>
template <typename SinkT>
PipelineBuilder<OrigIn, CurOut> with_sink(SinkT snk);
```

- `with_sink()`는 `add()` 뒤, `build()` 앞에 호출합니다.
- drain 코루틴이 마지막 스테이지 출력을 `snk.sink()`로 전달합니다.

### 4-3. MessageBusSource\<T\> / MessageBusSink\<T\>

MessageBus 토픽을 Pipeline Source/Sink로 연결합니다.

```cpp
// 토픽 "raw_orders" 구독 → Pipeline 소스
auto pipeline = PipelineBuilder<Order, Order>{}
    .with_source(MessageBusSource<Order>(bus, "raw_orders", 256))
    .add<ProcessedOrder>(stage_process)
    .with_sink(MessageBusSink<ProcessedOrder>(bus, "processed_orders"))
    .build();
```

### 4-4. SHMSource\<T\> / SHMSink\<T\>

프로세스 간 SHMChannel을 Pipeline Source/Sink로 연결합니다.

```cpp
// 다른 프로세스에서 쓰는 SHMChannel 읽기 → Pipeline 소스
auto pipeline = PipelineBuilder<RawMsg, RawMsg>{}
    .with_source(SHMSource<RawMsg>("ipc.ingress"))
    .add<ProcessedMsg>(stage_process)
    .with_sink(SHMSink<ProcessedMsg>("ipc.egress"))
    .build();
```

### 4-5. 완전 연계 예시 (거래 시스템)

```
[외부 피드 프로세스]
    ↓ SHMChannel<RawOrder>("trading.raw")
[SHMSource → with_source()]
    ↓ stage_parse → stage_enrich → stage_validate
[with_sink() → MessageBusSink → "validated_orders" 토픽]
    ↓ MessageBus fan-out
[MessageBusSource("validated_orders") → with_source()]
    ↓ DynamicPipeline (stage_risk_check → stage_record)
[output channel 소비]
```

```cpp
// Stage 1: SHM 입력 → MessageBus 출력
auto stage1 = PipelineBuilder<RawOrder, RawOrder>{}
    .with_source(SHMSource<RawOrder>("trading.raw"))
    .add<ParsedOrder>(stage_parse)
    .add<ParsedOrder>(stage_enrich)
    .add<ValidatedOrder>(stage_validate)
    .with_sink(MessageBusSink<ValidatedOrder>(bus, "validated_orders"))
    .build();

// Stage 2: MessageBus 입력 → DynamicPipeline
DynamicPipeline<ValidatedOrder> stage2;
stage2.add_stage("risk_check", stage_risk_check);
stage2.add_stage("record",     stage_record);

// MessageBus 브릿지 구독
auto bridge = bus.subscribe("validated_orders",
    [&](MessageBus::Msg msg, Context ctx) -> Task<Result<void>> {
        auto& v = std::any_cast<const ValidatedOrder&>(msg);
        co_return co_await stage2.push(v, ctx);
    });

stage1.start(dispatcher);
stage2.start(dispatcher);
```

---

## 5. Action 구현

### 5-1. 기본 서명

```cpp
// StaticPipeline 스테이지
Task<Result<Out>> my_stage(In input, ActionEnv env);

// DynamicPipeline 스테이지 (동종 타입)
Task<Result<T>> my_stage(T input, ActionEnv env);
```

### 5-2. ActionEnv 활용

```cpp
Task<Result<ParsedOrder>> stage_parse(RawOrder raw, ActionEnv env) {
    // Context 읽기
    auto trace = env.ctx.get<TraceContext>();

    // ServiceRegistry 접근
    auto& db = env.services.get<Database>();

    // Stop token 체크 (long-running 작업에서)
    if (env.stop.stop_requested())
        co_return std::unexpected(std::errc::operation_canceled);

    co_return ParsedOrder{...};
}
```

### 5-3. Action 설정

```cpp
typename Action<In, Out>::Config cfg {
    .workers    = 4,        // 병렬 워커 수 (기본 1)
    .queue_cap  = 512,      // 입력 채널 용량
};
builder.add<Out>(my_stage, cfg);
```

---

## 6. Performance & Scheduling

### 6-1. Bulkheading

CPU 집약 스테이지에 독립 worker pool을 할당하여 경량 스테이지 차단 방지.

```cpp
auto pipeline = PipelineBuilder<Frame, Frame>{}
    .add<Decoded>(stage_decode, {.workers = 1})   // 경량
    .add<Result>(stage_npu_infer, {.workers = 8}) // NPU 집약 → 독립 pool
    .add<Output>(stage_postproc, {.workers = 2})  // 경량
    .build();
```

### 6-2. Non-blocking Yielding

`co_await`는 현재 코루틴을 suspend하고 CPU를 다른 worker에게 양보합니다.
I/O 대기 중에도 reactor가 다른 작업을 계속 처리합니다.

---

## 7. Pattern Catalog

### 7-1. Fan-out (Broadcast)

```cpp
// MessageBus subscribe는 자동 fan-out
bus.subscribe("events", handler_a);
bus.subscribe("events", handler_b);
bus.subscribe("events", handler_c);
// publish → 세 핸들러 모두 호출
co_await bus.publish("events", event);
```

### 7-2. Fan-in (Merge)

```cpp
// 공유 채널에 여러 생산자가 push
auto shared_ch = std::make_shared<AsyncChannel<Item>>(256);

dispatcher.spawn(producer_a(shared_ch));  // MPMC — 안전
dispatcher.spawn(producer_b(shared_ch));
dispatcher.spawn(consumer(shared_ch));
```

### 7-3. DAG (Directed Acyclic Graph)

```cpp
PipelineGraph graph;
auto a = graph.add_stage("parse",    stage_parse);
auto b = graph.add_stage("enrich",   stage_enrich);
auto c = graph.add_stage("validate", stage_validate);
auto d = graph.add_stage("record",   stage_record);

graph.connect(a, b);
graph.connect(b, c);
graph.connect(c, d);
graph.build_and_start(dispatcher);
```

### 7-4. Feedback Loop

```cpp
// 실패 시 재시도 큐로 라우팅
Task<Result<Item>> stage_retry_gate(Item item, ActionEnv env) {
    if (item.attempt < 3) {
        // 재시도 파이프라인으로 재투입
        co_await retry_pipeline.push(item);
    }
    co_return item;
}
```

---

## 8. Applied Recipes

### Recipe A: 실시간 주문 처리 (SHM → Pipeline → MessageBus)

```cpp
struct RawOrder { uint64_t id; char symbol[16]; double price; int qty; };
// static_assert(std::is_trivially_copyable_v<RawOrder>);

auto pipeline = PipelineBuilder<RawOrder, RawOrder>{}
    .with_source(SHMSource<RawOrder>("exchange.raw_orders"))
    .add<ParsedOrder>(stage_parse)          // char[16] → std::string 변환
    .add<ParsedOrder>(stage_enrich)         // 심볼 정규화
    .add<ValidatedOrder>(stage_validate)    // notional 계산
    .with_sink(MessageBusSink<ValidatedOrder>(bus, "risk.orders"))
    .build();
```

### Recipe B: Sensor Fusion (N:1 Sync)

```cpp
// ServiceRegistry에 partial data 누적
Task<Result<FusedData>> stage_fuse(SensorData data, ActionEnv env) {
    auto& store = env.services.get<FusionStore>();
    store.add(data);
    if (!store.is_complete()) co_return std::unexpected(errc::in_progress);
    co_return store.compute_fusion();
}
```

### Recipe C: NPU 배치 처리

```cpp
// WorkerLocal 버퍼로 N개 프레임 누적 후 배치 추론
Task<Result<InferResult>> stage_npu(Frame frame, ActionEnv env) {
    auto& buf = env.worker_local<std::vector<Frame>>();
    buf.push_back(frame);
    if (buf.size() < 8) co_return std::unexpected(errc::in_progress);
    auto result = co_await npu_infer_batch(buf);
    buf.clear();
    co_return result;
}
```

### Recipe D: Dead Letter Queue

```cpp
Task<Result<Item>> stage_safe(Item item, ActionEnv env) {
    auto res = co_await risky_operation(item);
    if (!res) {
        co_await dlq_pipeline.push(DeadLetter{item, res.error()});
        co_return std::unexpected(res.error());
    }
    co_return *res;
}
```

---

## 9. Periodic Polling Sources

센서, 하드웨어 레지스터 등 push 방식 입력 처리.

```cpp
// 직접 코루틴 루프 (with_source 없이)
dispatcher.spawn([&]() mutable -> Task<void> {
    for (;;) {
        if (auto data = sensor.read(); data.has_value()) {
            co_await pipeline.push(*data);
        }
        co_await qbuem::sleep(1ms);
    }
}());

// 특정 코어에 고정 (NUMA 최적화)
dispatcher.spawn_on(0, sensor_loop());
```

---

## 10. 메시징 레이어 선택 가이드

| 상황 | 선택 |
| :--- | :--- |
| 동일 프로세스 스레드 간 | `AsyncChannel<T>` 직접 또는 `MessageBus` |
| 동일 프로세스, 토픽 기반 fan-out | `MessageBus` |
| 별도 프로세스 간 (IPC) | `SHMChannel<T>` 직접 또는 `SHMBus(SYSTEM_WIDE)` |
| Pipeline에 IPC 연결 | `SHMSource<T>` / `SHMSink<T>` + `with_source/sink` |
| Pipeline에 MessageBus 연결 | `MessageBusSource<T>` / `MessageBusSink<T>` |
| 다중 레이어 체인 | Stage 1 파이프라인 → MessageBusSink → MessageBus → Stage 2 |

---

*qbuem-stack — 고성능 데이터 엔지니어링의 최적 인프라.*
