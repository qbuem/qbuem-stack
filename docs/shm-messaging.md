# SHM Messaging: Zero-copy Inter-Process Communication

A detailed engineering guide to the shared memory (SHM) messaging infrastructure of `qbuem-stack`.
Covers the API reference based on the actual implementation and Pipeline integration patterns.

---

## 1. Messaging Layer Architecture

qbuem-stack provides three independent messaging layers, all fully interconnected via Pipeline bridge adapters.

| Layer | Implementation | Scope | Latency (p99) | Key Characteristics |
| :--- | :--- | :--- | :--- | :--- |
| **MessageBus** | `AsyncChannel<T>` (internal) | Intra-process | < 50ns | `std::any` type erasure, topic-based pub/sub, fan-out |
| **SHMBus (LOCAL_ONLY)** | `AsyncChannel<T>` | Intra-process | < 50ns | `SHMBus::declare<T>()` unified API |
| **SHMBus (SYSTEM_WIDE)** | `SHMChannel<T>` | Inter-process | < 150ns | POSIX SHM (`shm_open`+`mmap`), lock-free MPMC |
| **SHMChannel (direct)** | `SHMChannel<T>` | Inter-process | < 150ns | Lowest latency, `trivially_copyable` requirement |

---

## 2. SHMChannel\<T\>

### 2.1 Overview

`SHMChannel<T>` is a lock-free Vyukov MPMC ring buffer operating on top of POSIX shared memory.

```
namespace qbuem::shm
```

**Core constraint**: `T` must satisfy `std::is_trivially_copyable_v<T>`.
Types that involve heap allocation, such as `std::string` or `std::vector`, cannot be used.

```cpp
// ✅ Correct: fixed-size POD
struct SensorReading {
    uint64_t timestamp_ns;
    float    value;
    uint32_t sensor_id;
};
static_assert(std::is_trivially_copyable_v<SensorReading>);

// ✅ When strings are needed: use fixed-size arrays
struct RawOrder {
    uint64_t order_id;
    char     symbol[16];  // std::string not allowed — SHM requirement
    double   price;
    int      qty;
};
static_assert(std::is_trivially_copyable_v<RawOrder>);

// ❌ Incorrect: cannot be used in SHM
struct BadMsg {
    std::string name;   // static_assert fails
    std::vector<int> data;
};
```

### 2.2 API Reference

```cpp
template <typename T>
class SHMChannel {
public:
    using Ptr = std::shared_ptr<SHMChannel<T>>;

    // ── Create / Open ─────────────────────────────────────────────────────

    /// Producer side: creates a new SHM segment.
    /// @param name     Channel name (e.g., "trading.raw_orders")
    /// @param capacity Number of ring buffer slots (power of two recommended)
    static Result<Ptr> create(std::string_view name, size_t capacity) noexcept;

    /// Consumer side: opens an existing SHM segment.
    static Result<Ptr> open(std::string_view name) noexcept;

    /// Removes the SHM name from the filesystem (idempotent — ENOENT → ok).
    /// Actual memory is released by the kernel after all handles are close()d.
    static Result<void> unlink(std::string_view name) noexcept;

    // ── Producer API ─────────────────────────────────────────────────────

    /// Send a message (co_await backpressure when slots are saturated).
    Task<Result<void>> send(const T& msg) noexcept;

    /// Non-blocking send attempt. Returns false on failure.
    bool try_send(const T& msg) noexcept;

    // ── Consumer API ─────────────────────────────────────────────────────

    /// Receive a message (zero-copy — returns a direct DataArena pointer).
    /// Pointer is valid until the next recv() call.
    Task<std::optional<const T*>> recv() noexcept;

    /// Non-blocking receive attempt.
    std::optional<const T*> try_recv() noexcept;

    // ── Lifecycle ────────────────────────────────────────────────────────

    void close() noexcept;       ///< Transition to draining state
    bool is_open() const noexcept;
    size_t size() const noexcept;
};
```

### 2.3 Memory Layout (Segment Layout)

```
[Control Header (64B, cache-line aligned)]
  ├─ atomic<u64> tail     — producer commit index
  ├─ atomic<u64> head     — consumer consumption index
  ├─ u32 capacity         — number of ring buffer slots
  ├─ u32 magic            — 0x5142554D ("QBUM")
  └─ atomic<u32> state    — bit0: Active, bit1: Draining

[Slot Ring (capacity × sizeof(Slot))]
  Each Slot: Vyukov sequence (atomic<u64>) + payload (T)

[Padding to page boundary]
```

### 2.4 Usage Example

```cpp
#include <qbuem/shm/shm_channel.hpp>
using namespace qbuem;
using namespace qbuem::shm;

// Producer process
auto chan = SHMChannel<SensorReading>::create("sensors.temp", 1024);
co_await (*chan)->send(SensorReading{timestamp_ns(), 37.5f, 1});

// Consumer process
auto chan = SHMChannel<SensorReading>::open("sensors.temp");
auto msg = co_await (*chan)->recv();
if (msg) process(**msg);

// Cleanup on shutdown
(*chan)->close();
SHMChannel<SensorReading>::unlink("sensors.temp");  // idempotent
```

---

## 3. SHMBus

`SHMBus` is a unified message bus that manages multiple topics through a single interface.
Supports up to 64 topics, with `TopicScope` for switching between local and system-wide scope.

```
namespace qbuem::shm
```

```cpp
enum class TopicScope { LOCAL_ONLY, SYSTEM_WIDE };

class SHMBus {
public:
    // Declare a topic
    template <typename T>
    void declare(std::string_view name, TopicScope scope, size_t capacity = 256);

    // Publish (try_publish: non-blocking)
    template <typename T>
    bool try_publish(std::string_view name, const T& msg);

    // Subscribe (ISubscription::recv() → co_await)
    template <typename T>
    std::unique_ptr<ISubscription<T>> subscribe(std::string_view name);
};
```

### 3.1 TopicScope Selection Criteria

| Situation | Recommended Scope |
| :--- | :--- |
| Communication between threads within the same process | `LOCAL_ONLY` (AsyncChannel backend) |
| Communication between separate processes | `SYSTEM_WIDE` (SHMChannel backend) |
| Microservice IPC | `SYSTEM_WIDE` |

---

## 4. Pipeline Bridge Adapters

Adapter classes that connect the Pipeline to the messaging layers.

### 4.1 SHMSource\<T\> / SHMSink\<T\>

Uses `SHMChannel<T>` as a Pipeline Source/Sink.
Defined in `include/qbuem/shm/shm_bus.hpp`.

```
namespace qbuem::shm
```

```cpp
template <typename T>
class SHMSource {
public:
    explicit SHMSource(std::string_view channel_name);  // owns the name (std::string)
    Result<void> init() noexcept;                       // calls SHMChannel<T>::open()
    Task<std::optional<const T*>> next();               // delegates to recv()
    void close();
};

template <typename T>
class SHMSink {
public:
    explicit SHMSink(std::string_view channel_name, size_t capacity = 1024);
    Result<void> init() noexcept;           // calls SHMChannel<T>::create()
    Task<Result<void>> sink(const T& msg);  // delegates to send()
};
```

> **Note**: The `name_` member is owned as `std::string` (using `std::string_view` would cause dangling from temporaries).

### 4.2 MessageBusSource\<T\> / MessageBusSink\<T\>

Uses `MessageBus` topics as a Pipeline Source/Sink.
Defined in `include/qbuem/pipeline/message_bus.hpp`.

```
namespace qbuem
```

```cpp
template <typename T>
class MessageBusSource {
public:
    MessageBusSource(MessageBus& bus, std::string topic, size_t cap = 256);
    Result<void> init() noexcept;               // calls bus.subscribe_stream<T>()
    Task<std::optional<const T*>> next();       // stream recv, value stored in buf_
    void close();
};

template <typename T>
class MessageBusSink {
public:
    MessageBusSink(MessageBus& bus, std::string topic);
    Result<void> init() noexcept;                    // no-op (always ok)
    Task<Result<void>> sink(const T& msg);           // bus.publish(topic_, msg)
};
```

### 4.3 PipelineBuilder::with_source() / with_sink()

Connect adapters directly in `PipelineBuilder`.
Implemented in `include/qbuem/pipeline/static_pipeline.hpp`.

```cpp
// Source requirement: init() → Result<void>, next() → Task<optional<const T*>>
template <typename SourceT>
PipelineBuilder<OrigIn, CurOut> with_source(SourceT src, size_t cap = 256);

// Sink requirement: init() → Result<void>, sink(const T&) → Task<Result<void>>
template <typename SinkT>
PipelineBuilder<OrigIn, CurOut> with_sink(SinkT snk);
```

**How it works:**
- `with_source()`: Creates an internal `AsyncChannel` and sets `head_=tail_=src_ch`. A source pump coroutine runs the `src.next()` → `src_ch.send()` loop.
- `with_sink()`: Captures the current `tail_` channel. A drain coroutine runs the `drain_ch.recv()` → `snk.sink()` loop.

---

## 5. Full Integration Patterns

### 5.1 SHMChannel → Pipeline → MessageBus (One-Way Chain)

```
[External Producer Process]
    ↓ SHMChannel<RawOrder> ("trading.raw")
[SHMSource] → PipelineBuilder::with_source()
    ↓ stage_parse → stage_enrich → stage_validate
[MessageBusSink] → PipelineBuilder::with_sink()
    ↓ MessageBus topic("validated_orders")
[Subscriber 1, 2, 3 ...]
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

### 5.2 MessageBus → Pipeline → MessageBus (Dual Bus Relay)

```cpp
// Stage 1: Pre-processing pipeline
auto stage1 = PipelineBuilder<RawOrder, RawOrder>{}
    .add<ParsedOrder>(stage_parse)
    .add<ValidatedOrder>(stage_validate)
    .with_sink(MessageBusSink<ValidatedOrder>(bus, "stage1_out"))
    .build();

// Stage 2: Post-processing pipeline (receives Stage 1 output via MessageBusSource)
auto stage2 = PipelineBuilder<ValidatedOrder, ValidatedOrder>{}
    .with_source(MessageBusSource<ValidatedOrder>(bus, "stage1_out"))
    .add<ValidatedOrder>(stage_risk_check)
    .add<ValidatedOrder>(stage_record)
    .build();

stage1.start(dispatcher);
stage2.start(dispatcher);
```

### 5.3 SHMBus (LOCAL_ONLY) → Pipeline Manual Bridge

```cpp
SHMBus shm_bus;
shm_bus.declare<RawOrder>("shm.orders", TopicScope::LOCAL_ONLY, 64);

auto pipeline = PipelineBuilder<RawOrder, RawOrder>{}
    .add<ParsedOrder>(stage_parse)
    .build();
pipeline.start(dispatcher);

// SHMBus subscription → pipeline.push() bridge
auto sub = shm_bus.subscribe<RawOrder>("shm.orders");
dispatcher.spawn([&, s = std::move(sub)]() mutable -> Task<void> {
    for (;;) {
        auto msg = co_await s->recv();
        if (!msg) break;
        co_await pipeline.push(**msg);
    }
}());

// Publish
shm_bus.try_publish("shm.orders", RawOrder{1001, "SAMSUNG", 72500.0, 10});
```

---

## 6. trivially_copyable Constraint and Workaround Patterns

`SHMChannel<T>` performs `static_assert(std::is_trivially_copyable_v<T>)` at compile time.

| Required Data | SHM Type | Pipeline Internal Type | Conversion Point |
| :--- | :--- | :--- | :--- |
| String | `char symbol[16]` | `std::string` | First Pipeline stage |
| Variable-length array | `float data[64]` | `std::vector<float>` | First Pipeline stage |
| Complex struct | ID only | Full struct | `ServiceRegistry` lookup |

```cpp
// SHM-only type (trivially copyable)
struct RawOrder {
    uint64_t order_id;
    char     symbol[16];   // std::string not allowed
    double   price;
    int      qty;
};
static_assert(std::is_trivially_copyable_v<RawOrder>);

// Pipeline internal type (standard C++ types)
struct ParsedOrder {
    uint64_t    order_id;
    std::string symbol;    // std::string is usable here
    double      price;
    int         qty;
    bool        valid;
};

// Conversion stage (first add())
Task<Result<ParsedOrder>> stage_parse(RawOrder raw, ActionEnv) {
    ParsedOrder p;
    p.symbol = std::string(raw.symbol);  // char[16] → std::string
    // ...
    co_return p;
}
```

---

## 7. SHMChannel Cleanup (unlink)

`SHMChannel<T>::unlink()` removes the filesystem name of the SHM segment.

- **Idempotent**: Returns `ok()` if the name does not exist (ENOENT → treated as success)
- **Actual memory release**: The kernel releases the memory after all processes have called `close()`
- **`/dev/shm` cleanup**: Safely removes segments left behind after a crash

```cpp
// Single cleanup (safe to call repeatedly due to idempotency)
SHMChannel<SensorReading>::unlink("sensors.temp");

// Pattern for clearing residual segments at application startup
const char* names[] = {"trading.raw", "trading.validated"};
for (auto n : names) {
    SHMChannel<RawOrder>::unlink(n);   // ENOENT → ok, errors ignored
}
```

---

## 8. Reliability & Error Handling

| Situation | Behavior |
| :--- | :--- |
| Producer crash (during write) | Consumer: validate `recv()` return value; sequence mismatch → skip slot |
| Consumer lag (backpressure) | `send()` co_await blocks (automatic backpressure when channel is saturated) |
| `try_send()` channel saturated | Returns `false` — caller decides how to handle |
| SHM segment not found (`open()`) | Returns `Result::error()` |
| `unlink()` already deleted | Returns `ok()` (idempotent) |
| `SHMSource::init()` failure | `with_source()` pump coroutine does not start (silent handling) |

---

## 9. Performance Characteristics

| Metric | Value |
| :--- | :--- |
| **IPC latency** | < 150ns (Producer commit → Consumer wake) |
| **Throughput** | 5M+ msg/s (small payloads) |
| **Hot-path heap allocation** | 0 (direct DataArena access) |
| **Max topics (SHMBus)** | 64 |
| **`T` size limit** | Practically at or below slot size (`sizeof(T)` recommended ≤ 256B) |

---

*qbuem-stack SHM Messaging — zero-copy messaging across process boundaries.*
