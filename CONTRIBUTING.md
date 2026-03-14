# qbuem-stack — Contributing Guide

## 브랜치 전략

| 브랜치 패턴 | 용도 |
|------------|------|
| `feat/<기능>` | 새 기능 |
| `fix/<버그>` | 버그 수정 |
| `docs/<문서>` | 문서 변경 |
| `pipeline/<컴포넌트>` | Pipeline 레이어 작업 (예: `pipeline/async-channel`) |
| `perf/<대상>` | 성능 최적화 |

일반적인 PR 절차는 표준 GitHub Flow를 따릅니다.

---

## 코어 설계 원칙

### 예외 금지

`Task<T>`의 `unhandled_exception()` 은 **`std::terminate()`** 를 호출합니다.
모든 처리 함수는 `Task<Result<T>>` 를 반환하고 예외를 값으로 전달해야 합니다.

```cpp
// 옳음: 에러를 Result로 반환
Task<Result<ParsedBody>> parse(HttpRequest req, std::stop_token st) {
    if (req.body().empty()) co_return std::unexpected(errc::invalid_argument);
    co_return ParsedBody{req.body()};
}

// 잘못됨: throw 는 프로세스를 종료시킴
Task<ParsedBody> parse(HttpRequest req) {
    if (req.body().empty()) throw std::invalid_argument("empty"); // FATAL
    co_return ParsedBody{req.body()};
}
```

### JSON 라이브러리 정책

qbuem-stack **코어는 JSON 라이브러리에 의존하지 않습니다.**

| 계층 | JSON 의존성 |
|------|------------|
| `qbuem::core` | 없음 |
| `qbuem::http` | 없음 — `body()`는 `std::string_view` raw bytes |
| `qbuem::pipeline` | 없음 — 아이템 타입은 서비스에서 정의 |
| `qbuem::qbuem` | 없음 |

애플리케이션에서는 qbuem-json, simdjson, nlohmann/json, glaze 등 원하는
라이브러리를 자유롭게 선택하여 `req.body()`를 파싱하면 됩니다.

### Cross-reactor 코루틴 재개

다른 reactor 스레드의 코루틴을 직접 `resume()` 하면 데이터 레이스가 발생합니다.
반드시 **waiter가 속한 reactor의 `post()`를 경유**해야 합니다.

```cpp
// 잘못됨: 데이터 레이스
waiter.handle.resume();

// 옳음: waiter 소속 reactor에 post로 전달
waiter.reactor->post([h = waiter.handle]() { h.resume(); });
```

---

## Pipeline 레이어 기여 가이드

### 파일 위치

```
include/qbuem/pipeline/   ← 헤더 (인터페이스 + 인라인 구현)
src/pipeline/             ← 비인라인 구현
tests/pipeline/           ← 단위 테스트
```

### Action 구현 규칙

1. 처리 함수 서명: `Task<Result<Out>>(In, std::stop_token)`
2. `stop_token` 은 모든 co_await 지점에서 체크
3. Scale-in: `atomic<size_t> target_workers` + 워커 인덱스 비교 사용 (poison pill 미사용)
4. 워커 spawn: `Dispatcher::spawn()` 또는 `spawn_on()` 경유

```cpp
// Action 처리 함수 예시
Task<Result<ProcessedItem>> my_process(RawItem item, std::stop_token st) {
    if (st.stop_requested()) co_return std::unexpected(errc::operation_canceled);

    auto result = co_await some_async_work(item);
    if (!result) co_return std::unexpected(result.error());

    co_return ProcessedItem{std::move(*result)};
}
```

### StaticPipeline vs DynamicPipeline 선택

| 상황 | 권장 |
|------|------|
| 타입이 컴파일 타임에 결정됨 | `StaticPipeline` |
| Config 파일로 구성 | `DynamicPipeline` |
| 런타임 로직 교체 필요 | `DynamicPipeline` + `hot_swap()` |
| 최고 성능 필요 | `StaticPipeline` |

### 트레이싱 규칙

- 트레이싱은 **선택적(opt-in)**: 비활성화 시 `NoopExporter`로 zero-overhead
- `TraceContext`는 thread-local로 전파 (아이템 타입 오염 없음)
- 미샘플링 아이템에 대해 `child_span()` 호출 금지 (Sampler 결과 먼저 확인)
- 처리 함수 내에서 `Reactor::get_current_trace_context()`로 접근

### 테스트 요구사항

Pipeline 컴포넌트는 다음을 모두 테스트해야 합니다:

- 정상 흐름 (단일 워커, 다중 워커)
- Backpressure (채널 포화 시 send 대기)
- EOS 전파 (close() 후 recv가 올바르게 종료)
- Scale-in / Scale-out (워커 수 동적 변경)
- Drain (진행 중인 아이템 완료 후 종료)
- Cross-reactor 시나리오 (멀티 스레드 Dispatcher)

---

## 코드 스타일

- C++20: concepts, coroutines, `std::span`, `std::format` 활용
- 외부 의존성 추가 금지 (OS syscall만 허용)
- `alignas(64)` — cache-line 경계 필요한 공유 데이터에 적용
- `[[nodiscard]]` — 반환값 무시 시 버그가 되는 모든 함수에 적용
- `[[likely]]` / `[[unlikely]]` — hot path 분기에만 (남용 금지)
