# Application Versatility: Media, AI, FinTech, Gaming, and Beyond

`qbuem-stack`은 범용 고성능 인프라 엔진입니다. 9-레벨 모듈러 아키텍처와 커널 네이티브 원칙은
전통적인 웹 서버를 넘어 다양한 고수요 분야에서 활용 가능합니다.

---

## 1. Media Streaming & Real-time Delivery

초고화질 영상/음성 스트리밍은 극단적인 처리량과 최소 지터를 요구합니다.

### 🚀 qbuem-stack 대응 방식

- **Zero-Copy Distribution (Level 3)**: `sendfile(2)` + `splice(2)`로 CPU 개입 없이 디스크 → NIC 직접 전송.
- **UDP / AF_XDP (Level 9)**: 저지연 라이브 스트리밍(RTP/SRT)에서 `AF_XDP`로 커널 네트워크 스택 우회, 수백만 PPS 달성.
- **Pipeline Transcoding (Level 6)**: `StaticPipeline`으로 비트스트림 세그멘팅 또는 AES 암호화를 멀티코어 병렬로 처리. Lock 없음.
- **Bulkhead Worker Pools**: 인코딩 스테이지에 전용 worker pool 할당 — 경량 패킷 처리가 중 연산에 블로킹되지 않음.

```cpp
// 미디어 트랜스코딩 파이프라인 예시
auto transcode = PipelineBuilder<RawFrame, RawFrame>{}
    .add<DecodedFrame>(stage_decode,  {.workers = 2})
    .add<ScaledFrame> (stage_scale,   {.workers = 4})  // SIMD 스케일링
    .add<EncodedFrame>(stage_encode,  {.workers = 8})  // 집약적 인코딩
    .add<Segment>     (stage_segment, {.workers = 2})
    .build();
```

---

## 2. AI & NPU Processing (Hardware Acceleration)

NPU/GPU는 낮은 레이턴시로 데이터를 공급받아야 컴퓨팅 유닛이 포화 상태를 유지합니다.

### 🧠 qbuem-stack 대응 방식

- **PCIe User-space Integration (Level 7)**: VFIO 기반 PCIe 직접 제어로 NPU 디바이스 메모리와 인터럽트를 user-space에서 관리.
- **SHM Messaging (Level 3)**: AI 모델이 별도 프로세스에서 실행될 때, `SHMChannel`로 복사 없이 sub-microsecond IPC.
- **Pipeline IPC Bridge**: `SHMSource<Frame>` → 전처리 파이프라인 → `SHMSink<InferResult>` — 추론 파이프라인 완전 분리.
- **Contextual Pipelines (Level 6)**: `Context`로 카메라 ID, 타임스탬프 등 메타데이터를 프레임에 부착하고, NPU 스테이지로 라우팅.

```cpp
// AI 추론 파이프라인: 카메라 SHM → 전처리 → NPU → 결과 발행
struct RawFrame { uint64_t ts; uint32_t cam_id; uint8_t data[1920*1080*3]; };

auto ai_pipeline = PipelineBuilder<RawFrame, RawFrame>{}
    .with_source(SHMSource<RawFrame>("camera.frames"))
    .add<PreprocessedFrame>(stage_preprocess, {.workers = 4})
    .add<InferResult>(stage_npu_infer, {.workers = 8})  // NPU 집약
    .with_sink(MessageBusSink<InferResult>(bus, "detections"))
    .build();
```

---

## 3. FinTech: 고빈도 거래 & 리스크 관리

나노초 단위 레이턴시와 완전한 감사 추적이 필요한 금융 시스템.

### 💹 qbuem-stack 대응 방식

- **SHM IPC Pipeline**: 외부 피드 프로세스 → `SHMChannel<RawOrder>` → Pipeline → `MessageBus` fan-out — 전체 체인 < 1μs.
- **StaticPipeline**: 컴파일 타임 타입 체인으로 zero-overhead 주문 처리 (parse → enrich → validate → risk).
- **MessageBus Fan-out**: 단일 주문 이벤트를 여러 구독자 (리스크 엔진, 기록 시스템, 모니터링)로 동시 전달.
- **CircuitBreaker + RetryPolicy**: 외부 거래소 연결 장애 시 자동 차단 및 복구.
- **JwtAuthAction**: 주문 요청에 대한 SIMD JWT 검증 + LRU 캐시 (< 100ns 검증).

```cpp
// 완전한 거래 처리 체인
// RawOrder (trivially copyable for SHM)
struct RawOrder { uint64_t id; char symbol[16]; double price; int qty; };

// Stage 1: 시장 데이터 피드 → 주문 검증
auto order_pipe = PipelineBuilder<RawOrder, RawOrder>{}
    .with_source(SHMSource<RawOrder>("exchange.raw_orders"))
    .add<ParsedOrder>  (stage_parse,    {.workers = 2})
    .add<ParsedOrder>  (stage_enrich,   {.workers = 1})  // 마스터 데이터 조회
    .add<ValidatedOrder>(stage_validate, {.workers = 2})
    .with_sink(MessageBusSink<ValidatedOrder>(bus, "risk.orders"))
    .build();

// Stage 2: 리스크 체크 + 기록
auto risk_pipe = PipelineBuilder<ValidatedOrder, ValidatedOrder>{}
    .with_source(MessageBusSource<ValidatedOrder>(bus, "risk.orders"))
    .add<ValidatedOrder>(stage_risk_check, {.workers = 4})
    .add<ValidatedOrder>(stage_record,     {.workers = 2})
    .build();
```

---

## 4. Industrial Edge & IoT

엣지 디바이스는 작은 footprint와 높은 신뢰성을 요구합니다.

### 🏭 qbuem-stack 대응 방식

- **Zero Dependency**: 외부 라이브러리 없이 bare Linux/macOS 커널 위에서 동작. 최소 임베디드 OS 이미지에 이상적.
- **Resilience Patterns (Level 7)**: `CircuitBreaker` + `RetryPolicy` — 간헐적 센서 데이터 또는 네트워크 단절에도 시스템 안정.
- **NUMA-aware Dispatching**: 고급 엣지 게이트웨이에서 처리 태스크를 물리 IO 포트 근처 CPU 코어에 고정.
- **SHMBus LOCAL_ONLY**: 동일 프로세스 내 센서 데이터 팬아웃, 힙 할당 없음.

```cpp
// 센서 데이터 수집 파이프라인
SHMBus shm_bus;
shm_bus.declare<SensorReading>("sensors.temp", TopicScope::LOCAL_ONLY, 64);

auto pipeline = PipelineBuilder<SensorReading, SensorReading>{}
    .add<ProcessedReading>(stage_filter)
    .add<Alert>(stage_anomaly_detect)
    .build();
pipeline.start(dispatcher);

// 센서 폴링 루프
dispatcher.spawn_on(0, [&]() -> Task<void> {  // 코어 0 고정
    for (;;) {
        shm_bus.try_publish("sensors.temp", read_sensor());
        co_await qbuem::sleep(100ms);
    }
}());
```

---

## 5. 실시간 게임 서버 (Multiplayer Game Backend)

멀티플레이어 게임 백엔드는 낮은 레이턴시, 높은 동시 접속자, 게임 상태의 일관성을 동시에 요구합니다.

### 🎮 qbuem-stack 대응 방식

- **StaticPipeline 게임 액션 체인**: `validate → apply → finalize` 3단계 비동기 파이프라인으로 게임 로직을 멀티코어에 분산. 각 스테이지가 독립 worker pool을 가지므로 CPU 바운드 연산(충돌 계산, AI)이 I/O를 블로킹하지 않음.
- **MessageBus 브로드캐스트**: 방(Room) 단위 SSE 스트리밍 — `bus.try_publish("room.{id}", event)` 한 번으로 해당 방 모든 구독자에게 팬아웃. 토픽 격리로 다른 방 클라이언트에게 누설 없음.
- **Context 전파**: `Context`에 `ResponseChannel`을 담아 파이프라인을 통과시키면, 마지막 스테이지에서 HTTP 응답을 직접 수행. Cross-reactor 안전.
- **DynamicPipeline 운영 유연성**: 배포 중단 없이 `hot_swap`으로 persist 스테이지 교체, `set_enabled`로 replay 버퍼를 런타임에 on/off.
- **RateLimit 치트 방지**: 플레이어당 초당 60 요청 제한. IP 또는 토큰 기반 키 격리 (`max_keys=10,000`).
- **SLO 추적**: `stage_finalize`에 SLO 설정으로 액션 처리 P99 레이턴시 모니터링.
- **자동 스케일러**: 500ms 주기로 큐 깊이를 감시하여 과부하 시 워커 추가 (`scale_out`), 여유 시 회수 (`scale_in`).

```
[HTTP Client] → Bearer Auth (game-key-{player})
    │
    ├─ POST /api/v1/rooms              방 생성
    ├─ POST /api/v1/rooms/:id/join     방 참가
    ├─ POST /api/v1/rooms/:id/action   게임 액션 제출
    ├─ GET  /api/v1/rooms/:id/events   SSE 실시간 이벤트
    └─ GET  /api/v1/leaderboard        플레이어 랭킹

[StaticPipeline]
    Action<GameAction, ValidatedAction>  validate  {slo: p99 < 1ms}
    Action<ValidatedAction, StateUpdate> apply     {slo: p99 < 3ms}
    Action<StateUpdate, GameEvent>       finalize

[DynamicPipeline<GameEvent>]
    Stage "persist"   — 이벤트 히스토리 저장 (hot_swap 가능)
    Stage "replay"    — 리플레이 버퍼 적재 (toggle 가능)
    Stage "notify"    — ResponseChannel + MessageBus 팬아웃

[MessageBus]
    Topic "room.{id}" → 방별 SSE 스트리밍
    Topic "leaderboard" → 게임 종료 시 랭킹 갱신 SSE
```

```cpp
// 게임 액션 Nexus Fusion 역직렬화 (zero-tape, 50~230ns)
inline void nexus_pulse(std::string_view key, const char*& p,
                        const char* end, GameAction& g) {
    using namespace qbuem::json::detail;
    switch (fnv1a_hash(key)) {
        case fnv1a_hash_ce("power"):   from_json_direct(p, end, g.power);   break;
        case fnv1a_hash_ce("action"): {
            std::string s; from_json_direct(p, end, s);
            if      (s == "defend")  g.type = ActionType::Defend;
            else if (s == "special") g.type = ActionType::Special;
            else                     g.type = ActionType::Attack;
            break;
        }
        default: skip_direct(p, end); break;
    }
}

// 게임 액션 제출 → 파이프라인 처리 → HTTP 응답
auto resp_ch = server.submit_action(action, ctx);
auto ev = co_await resp_ch->recv();   // 파이프라인 완료까지 비동기 대기

// 방별 SSE 실시간 이벤트 스트리밍
auto stream = bus.subscribe_stream<GameEvent>("room.1", 64);
for (int i = 0; i < 100; ++i) {
    auto ev = co_await stream->recv();
    sse.send(qbuem::write(*ev), ev->game_over ? "game_over" : "action");
    if (ev->game_over) break;
}
```

> **예제 파일**: [`examples/game_server.cpp`](../examples/game_server.cpp)
>
> ```bash
> # 빌드
> cmake -DQBUEM_BUILD_EXAMPLES=ON -DQBUEM_JSON_BUILD_BENCHMARKS=OFF ..
> make game_server
>
> # 방 생성 → 참가 → 액션
> curl -H 'Authorization: Bearer game-key-alice' \
>      -X POST http://localhost:8080/api/v1/rooms \
>      -d '{"room_name":"arena-1"}'
>
> curl -H 'Authorization: Bearer game-key-bob' \
>      -X POST http://localhost:8080/api/v1/rooms/1/join
>
> curl -H 'Authorization: Bearer game-key-alice' \
>      -X POST http://localhost:8080/api/v1/rooms/1/action \
>      -d '{"action":"attack","power":8}'
>
> # SSE 이벤트 스트리밍
> curl -N http://localhost:8080/api/v1/rooms/1/events
> ```

### 성능 특성

| 지표 | 값 | 비고 |
| :--- | :--- | :--- |
| 액션 처리 P99 | < 5ms | validate + apply + finalize 합산 |
| SSE 팬아웃 지연 | < 500μs | MessageBus try_publish |
| 동시 방 수 | 수만 개 | 인메모리 GameRegistry, mutex 경합 최소화 |
| 치트 방지 | 60 req/s | RateLimit 미들웨어 |

---

## 6. gRPC 마이크로서비스

프로세스 간 gRPC 통신을 Pipeline과 연계합니다.

### gRPC + Pipeline 통합

```cpp
// gRPC 핸들러가 Pipeline으로 요청 라우팅
auto handler = GrpcHandler<OrderRequest, OrderResponse>{
    [&](Stream<OrderRequest> in, Stream<OrderResponse> out) -> Task<void> {
        async for (auto& req : in) {
            co_await order_pipeline.push(req);
        }
    }
};
```

---

## 6. Summary: The Universal Advantage

| Field | Key qbuem-stack Feature | 핵심 구성요소 |
| :--- | :--- | :--- |
| **Media** | Zero-copy `sendfile`, `AF_XDP`, Stream Pipelines | `zerocopy`, `xdp`, `pipeline` |
| **AI/ML** | SHM IPC, PCIe VFIO, Pipeline Bridge | `shm`, `pcie`, `SHMSource/Sink` |
| **FinTech** | Nano-second latency, Lock-free MPMC, Full IPC chain | `SHMChannel`, `MessageBus`, `pipeline` |
| **Gaming** | StaticPipeline 액션 처리, MessageBus SSE 팬아웃, Context 전파 | `StaticPipeline`, `MessageBus`, `SSE`, `DynamicPipeline` |
| **IoT** | Zero-dependency, Small binary, NUMA-aware | `reactor`, `shm`, `resilience` |
| **Telecom** | AF_XDP 100M+ PPS, QUIC/HTTP3 | `xdp`, `http2` |

---

*qbuem-stack — 플랫폼 범용성을 위한 엔지니어링.*
