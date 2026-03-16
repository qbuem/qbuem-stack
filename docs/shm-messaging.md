# SHM Messaging: Zero-copy Inter-Process Communication

`qbuem-stack`의 공유 메모리(SHM) 메시징 인프라에 대한 상세 엔지니어링 가이드입니다.
실제 구현 기반의 API 레퍼런스와 Pipeline 연계 패턴을 다룹니다.

---

## 1. 메시징 계층 아키텍처

qbuem-stack은 세 가지 독립적인 메시징 레이어를 제공하며, 이들은 Pipeline 브릿지 어댑터를 통해 완전히 연계됩니다.

| 레이어 | 구현체 | 스코프 | 레이턴시 (p99) | 주요 특성 |
| :--- | :--- | :--- | :--- | :--- |
| **MessageBus** | `AsyncChannel<T>` (내부) | 프로세스 내 (intra-process) | < 50ns | `std::any` 타입 소거, 토픽 기반 pub/sub, fan-out |
| **SHMBus (LOCAL_ONLY)** | `AsyncChannel<T>` | 프로세스 내 | < 50ns | `SHMBus::declare<T>()` 통합 API |
| **SHMBus (SYSTEM_WIDE)** | `SHMChannel<T>` | 프로세스 간 (inter-process) | < 150ns | POSIX SHM (`shm_open`+`mmap`), lock-free MPMC |
| **SHMChannel (직접)** | `SHMChannel<T>` | 프로세스 간 | < 150ns | 최저 레이턴시, `trivially_copyable` 요건 |

---

## 2. SHMChannel\<T\>

### 2.1 개요

`SHMChannel<T>`는 POSIX 공유 메모리 위에서 동작하는 lock-free Vyukov MPMC ring buffer입니다.

```
namespace qbuem::shm
```

**핵심 제약사항**: `T`는 `std::is_trivially_copyable_v<T>`를 만족해야 합니다.
`std::string`, `std::vector` 등 heap을 포함하는 타입은 사용 불가합니다.

```cpp
// ✅ 올바른 예: 고정 크기 POD
struct SensorReading {
    uint64_t timestamp_ns;
    float    value;
    uint32_t sensor_id;
};
static_assert(std::is_trivially_copyable_v<SensorReading>);

// ✅ 문자열이 필요한 경우: 고정 크기 배열 사용
struct RawOrder {
    uint64_t order_id;
    char     symbol[16];  // std::string 불가 — SHM 요건
    double   price;
    int      qty;
};
static_assert(std::is_trivially_copyable_v<RawOrder>);

// ❌ 잘못된 예: SHM에서 사용 불가
struct BadMsg {
    std::string name;   // static_assert 실패
    std::vector<int> data;
};
```

### 2.2 API 레퍼런스

```cpp
template <typename T>
class SHMChannel {
public:
    using Ptr = std::shared_ptr<SHMChannel<T>>;

    // ── 생성 / 열기 ──────────────────────────────────────────────────────

    /// Producer 측: SHM 세그먼트를 새로 생성합니다.
    /// @param name     채널 이름 (예: "trading.raw_orders")
    /// @param capacity 링버퍼 슬롯 수 (2의 거듭제곱 권장)
    static Result<Ptr> create(std::string_view name, size_t capacity) noexcept;

    /// Consumer 측: 기존 SHM 세그먼트를 엽니다.
    static Result<Ptr> open(std::string_view name) noexcept;

    /// SHM 이름을 파일시스템에서 제거합니다 (멱등 — ENOENT → ok).
    /// 모든 핸들이 close()된 뒤 실제 메모리가 해제됩니다.
    static Result<void> unlink(std::string_view name) noexcept;

    // ── 생산자 API ───────────────────────────────────────────────────────

    /// 메시지 전송 (슬롯 포화 시 co_await 백프레셔).
    Task<Result<void>> send(const T& msg) noexcept;

    /// 논블로킹 송신 시도. 실패 시 false 반환.
    bool try_send(const T& msg) noexcept;

    // ── 소비자 API ───────────────────────────────────────────────────────

    /// 메시지 수신 (zero-copy — DataArena 직접 포인터 반환).
    /// 다음 recv() 호출 전까지 포인터 유효.
    Task<std::optional<const T*>> recv() noexcept;

    /// 논블로킹 수신 시도.
    std::optional<const T*> try_recv() noexcept;

    // ── 라이프사이클 ─────────────────────────────────────────────────────

    void close() noexcept;       ///< Draining 상태로 전환
    bool is_open() const noexcept;
    size_t size() const noexcept;
};
```

### 2.3 메모리 구조 (세그먼트 레이아웃)

```
[Control Header (64B, cache-line aligned)]
  ├─ atomic<u64> tail     — 생산자 커밋 인덱스
  ├─ atomic<u64> head     — 소비자 소비 인덱스
  ├─ u32 capacity         — 링버퍼 슬롯 수
  ├─ u32 magic            — 0x5142554D ("QBUM")
  └─ atomic<u32> state    — bit0: Active, bit1: Draining

[Slot Ring (capacity × sizeof(Slot))]
  각 Slot: Vyukov sequence (atomic<u64>) + payload (T)

[Padding to page boundary]
```

### 2.4 사용 예시

```cpp
#include <qbuem/shm/shm_channel.hpp>
using namespace qbuem;
using namespace qbuem::shm;

// Producer 프로세스
auto chan = SHMChannel<SensorReading>::create("sensors.temp", 1024);
co_await (*chan)->send(SensorReading{timestamp_ns(), 37.5f, 1});

// Consumer 프로세스
auto chan = SHMChannel<SensorReading>::open("sensors.temp");
auto msg = co_await (*chan)->recv();
if (msg) process(**msg);

// 종료 시 정리
(*chan)->close();
SHMChannel<SensorReading>::unlink("sensors.temp");  // 멱등
```

---

## 3. SHMBus

`SHMBus`는 여러 토픽을 하나의 인터페이스로 관리하는 통합 메시지 버스입니다.
최대 64개 토픽, `TopicScope`로 로컬/시스템 전역 전환.

```
namespace qbuem::shm
```

```cpp
enum class TopicScope { LOCAL_ONLY, SYSTEM_WIDE };

class SHMBus {
public:
    // 토픽 선언
    template <typename T>
    void declare(std::string_view name, TopicScope scope, size_t capacity = 256);

    // 발행 (try_publish: 논블로킹)
    template <typename T>
    bool try_publish(std::string_view name, const T& msg);

    // 구독 (ISubscription::recv() → co_await)
    template <typename T>
    std::unique_ptr<ISubscription<T>> subscribe(std::string_view name);
};
```

### 3.1 TopicScope 선택 기준

| 상황 | 권장 Scope |
| :--- | :--- |
| 동일 프로세스 내 스레드 간 통신 | `LOCAL_ONLY` (AsyncChannel 백엔드) |
| 별도 프로세스 간 통신 | `SYSTEM_WIDE` (SHMChannel 백엔드) |
| 마이크로서비스 IPC | `SYSTEM_WIDE` |

---

## 4. Pipeline 브릿지 어댑터

Pipeline과 메시징 레이어를 연결하는 어댑터 클래스들입니다.

### 4.1 SHMSource\<T\> / SHMSink\<T\>

`SHMChannel<T>`를 Pipeline Source/Sink로 사용합니다.
`include/qbuem/shm/shm_bus.hpp`에 정의되어 있습니다.

```
namespace qbuem::shm
```

```cpp
template <typename T>
class SHMSource {
public:
    explicit SHMSource(std::string_view channel_name);  // 이름 소유 (std::string)
    Result<void> init() noexcept;                       // SHMChannel<T>::open() 호출
    Task<std::optional<const T*>> next();               // recv() 위임
    void close();
};

template <typename T>
class SHMSink {
public:
    explicit SHMSink(std::string_view channel_name, size_t capacity = 1024);
    Result<void> init() noexcept;           // SHMChannel<T>::create() 호출
    Task<Result<void>> sink(const T& msg);  // send() 위임
};
```

> **주의**: `name_` 멤버는 `std::string`으로 소유합니다 (`std::string_view` 사용 시 임시 객체에서 dangling).

### 4.2 MessageBusSource\<T\> / MessageBusSink\<T\>

`MessageBus` 토픽을 Pipeline Source/Sink로 사용합니다.
`include/qbuem/pipeline/message_bus.hpp`에 정의되어 있습니다.

```
namespace qbuem
```

```cpp
template <typename T>
class MessageBusSource {
public:
    MessageBusSource(MessageBus& bus, std::string topic, size_t cap = 256);
    Result<void> init() noexcept;               // bus.subscribe_stream<T>() 호출
    Task<std::optional<const T*>> next();       // stream recv, 값은 buf_에 저장
    void close();
};

template <typename T>
class MessageBusSink {
public:
    MessageBusSink(MessageBus& bus, std::string topic);
    Result<void> init() noexcept;                    // no-op (항상 ok)
    Task<Result<void>> sink(const T& msg);           // bus.publish(topic_, msg)
};
```

### 4.3 PipelineBuilder::with_source() / with_sink()

`PipelineBuilder`에서 직접 어댑터를 연결합니다.
`include/qbuem/pipeline/static_pipeline.hpp`에 구현되어 있습니다.

```cpp
// Source 요건: init() → Result<void>, next() → Task<optional<const T*>>
template <typename SourceT>
PipelineBuilder<OrigIn, CurOut> with_source(SourceT src, size_t cap = 256);

// Sink 요건: init() → Result<void>, sink(const T&) → Task<Result<void>>
template <typename SinkT>
PipelineBuilder<OrigIn, CurOut> with_sink(SinkT snk);
```

**동작 원리:**
- `with_source()`: 내부 `AsyncChannel`을 생성하고 `head_=tail_=src_ch`로 설정. 소스 펌프 코루틴이 `src.next()` → `src_ch.send()` 루프를 실행.
- `with_sink()`: 현재 `tail_` 채널을 캡처. drain 코루틴이 `drain_ch.recv()` → `snk.sink()` 루프를 실행.

---

## 5. 완전 연계 패턴

### 5.1 SHMChannel → Pipeline → MessageBus (단방향 체인)

```
[외부 Producer 프로세스]
    ↓ SHMChannel<RawOrder> ("trading.raw")
[SHMSource] → PipelineBuilder::with_source()
    ↓ stage_parse → stage_enrich → stage_validate
[MessageBusSink] → PipelineBuilder::with_sink()
    ↓ MessageBus topic("validated_orders")
[구독자 1, 2, 3 ...]
```

```cpp
using namespace qbuem;
using namespace qbuem::shm;

MessageBus bus;
bus.start(dispatcher);

auto pipeline = PipelineBuilder<RawOrder, RawOrder>{}
    .with_source(SHMSource<RawOrder>("trading.raw"))
    .add<ParsedOrder>(stage_parse)
    .add<ParsedOrder>(stage_enrich)
    .add<ValidatedOrder>(stage_validate)
    .with_sink(MessageBusSink<ValidatedOrder>(bus, "validated_orders"))
    .build();

pipeline.start(dispatcher);
```

### 5.2 MessageBus → Pipeline → MessageBus (이중 버스 릴레이)

```cpp
// Stage 1: 전처리 파이프라인
auto stage1 = PipelineBuilder<RawOrder, RawOrder>{}
    .add<ParsedOrder>(stage_parse)
    .add<ValidatedOrder>(stage_validate)
    .with_sink(MessageBusSink<ValidatedOrder>(bus, "stage1_out"))
    .build();

// Stage 2: 후처리 파이프라인 (MessageBusSource로 Stage 1 출력 수신)
auto stage2 = PipelineBuilder<ValidatedOrder, ValidatedOrder>{}
    .with_source(MessageBusSource<ValidatedOrder>(bus, "stage1_out"))
    .add<ValidatedOrder>(stage_risk_check)
    .add<ValidatedOrder>(stage_record)
    .build();

stage1.start(dispatcher);
stage2.start(dispatcher);
```

### 5.3 SHMBus (LOCAL_ONLY) → Pipeline 수동 브릿지

```cpp
SHMBus shm_bus;
shm_bus.declare<RawOrder>("shm.orders", TopicScope::LOCAL_ONLY, 64);

auto pipeline = PipelineBuilder<RawOrder, RawOrder>{}
    .add<ParsedOrder>(stage_parse)
    .build();
pipeline.start(dispatcher);

// SHMBus 구독 → pipeline.push() 브릿지
auto sub = shm_bus.subscribe<RawOrder>("shm.orders");
dispatcher.spawn([&, s = std::move(sub)]() mutable -> Task<void> {
    for (;;) {
        auto msg = co_await s->recv();
        if (!msg) break;
        co_await pipeline.push(**msg);
    }
}());

// 발행
shm_bus.try_publish("shm.orders", RawOrder{1001, "SAMSUNG", 72500.0, 10});
```

---

## 6. trivially_copyable 제약과 대응 패턴

`SHMChannel<T>`는 컴파일 타임에 `static_assert(std::is_trivially_copyable_v<T>)`를 수행합니다.

| 필요 데이터 | SHM 타입 | Pipeline 내부 타입 | 변환 위치 |
| :--- | :--- | :--- | :--- |
| 문자열 | `char symbol[16]` | `std::string` | 첫 번째 Pipeline 스테이지 |
| 가변 길이 배열 | `float data[64]` | `std::vector<float>` | 첫 번째 Pipeline 스테이지 |
| 복잡한 구조체 | ID만 포함 | 풀 구조체 | `ServiceRegistry` 조회 |

```cpp
// SHM 전용 타입 (trivially copyable)
struct RawOrder {
    uint64_t order_id;
    char     symbol[16];   // std::string 불가
    double   price;
    int      qty;
};
static_assert(std::is_trivially_copyable_v<RawOrder>);

// Pipeline 내부 타입 (일반 C++ 타입)
struct ParsedOrder {
    uint64_t    order_id;
    std::string symbol;    // std::string 사용 가능
    double      price;
    int         qty;
    bool        valid;
};

// 변환 스테이지 (첫 번째 add())
Task<Result<ParsedOrder>> stage_parse(RawOrder raw, ActionEnv) {
    ParsedOrder p;
    p.symbol = std::string(raw.symbol);  // char[16] → std::string
    // ...
    co_return p;
}
```

---

## 7. SHMChannel 정리 (unlink)

`SHMChannel<T>::unlink()`은 SHM 세그먼트의 파일시스템 이름을 제거합니다.

- **멱등**: 이미 존재하지 않는 이름이면 `ok()` 반환 (ENOENT → 성공 처리)
- **실제 메모리 해제**: 모든 프로세스가 `close()`한 뒤 커널이 메모리 해제
- **`/dev/shm` 정리**: crash 후 남은 세그먼트를 안전하게 제거

```cpp
// 단일 정리 (멱등이므로 반복 호출 안전)
SHMChannel<SensorReading>::unlink("sensors.temp");

// 애플리케이션 시작 시 잔여 세그먼트 초기화 패턴
const char* names[] = {"trading.raw", "trading.validated"};
for (auto n : names) {
    SHMChannel<RawOrder>::unlink(n);   // ENOENT → ok, 오류 무시
}
```

---

## 8. 신뢰성 & 오류 처리

| 상황 | 동작 |
| :--- | :--- |
| Producer crash (쓰기 중) | Consumer: `recv()` 반환값 검증, Sequence 불일치 → 슬롯 스킵 |
| Consumer 지연 (backpressure) | `send()` co_await 대기 (채널 포화 시 자동 백프레셔) |
| `try_send()` 채널 포화 | `false` 반환 — 호출자가 처리 결정 |
| SHM 세그먼트 없음 (`open()`) | `Result::error()` 반환 |
| `unlink()` 이미 삭제됨 | `ok()` 반환 (멱등) |
| `SHMSource::init()` 실패 | `with_source()` 펌프 코루틴 시작 안 함 (무음 처리) |

---

## 9. 성능 특성

| 지표 | 값 |
| :--- | :--- |
| **IPC 레이턴시** | < 150ns (Producer commit → Consumer wake) |
| **처리량** | 5M+ msg/s (소형 페이로드) |
| **hot-path 힙 할당** | 0 (DataArena 직접 접근) |
| **최대 토픽 수 (SHMBus)** | 64 |
| **`T` 크기 제한** | 실용적으로 슬롯 크기 이하 (`sizeof(T)` 권장 ≤ 256B) |

---

*qbuem-stack SHM Messaging — 프로세스 경계를 넘는 zero-copy 메시징.*
