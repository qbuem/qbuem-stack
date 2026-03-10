# Draco WAS — Contributing Guide

## JSON 라이브러리 정책

Draco WAS 프레임워크 **코어는 JSON 라이브러리에 의존하지 않습니다.**

| 계층 | JSON 의존성 |
|------|------------|
| `draco::core` | ❌ 없음 |
| `draco::http` | ❌ 없음 — `body()`는 `std::string_view` raw bytes |
| `draco::draco` | ❌ 없음 |
| `examples/` | ✅ **beast_json 전용** |

### 예제에서 beast_json을 사용하는 이유

예제는 단일 JSON 라이브러리를 기준으로 작성하여 품질을 보장합니다.
beast_json은 아직 성숙 단계에 있으며, **예제를 통해 실전 사용 피드백을 수집하고
발견된 버그를 upstream에 기여하는 것이 Draco의 방침입니다.**

애플리케이션에서는 simdjson, nlohmann/json, glaze 등 원하는 라이브러리를 자유롭게
선택하여 `req.body()`를 파싱하면 됩니다.

---

## beast_json 버그 리포팅 가이드

예제를 실행하다 beast_json에서 버그를 발견하면 **아래 형식**으로 리포트합니다.

**리포팅 위치:** https://github.com/the-lkb/beast-json/issues

### 이슈 제목 형식

```
[Bug] <한 줄 요약>  (발견: draco-was examples)
```

예시:
```
[Bug] parse() throws on trailing comma in object literal  (발견: draco-was examples)
```

### 이슈 본문 템플릿

````markdown
## 환경

| 항목 | 값 |
|------|----|
| beast_json 버전 / 커밋 | `git rev-parse HEAD` 결과 |
| 컴파일러 | clang 17 / gcc 13 등 |
| OS | Ubuntu 24.04 / macOS 14 등 |
| draco-was 커밋 | `git rev-parse HEAD` 결과 |

## 재현 코드 (최소 예시)

```cpp
#include <beast_json/beast_json.hpp>
#include <cassert>

int main() {
    beast::Document doc;
    // ← 버그를 재현하는 최소 코드
    auto v = beast::parse(doc, R"({"key": 1,})"); // trailing comma
    // 기대: 예외 또는 오류 반환
    // 실제: ???
}
```

## 빌드 & 실행 방법

```bash
git clone https://github.com/the-lkb/beast-json.git
cd beast-json
# CMake 빌드 방법 작성
```

## 기대 동작

<!-- RFC 또는 라이브러리 문서 기준으로 기대하는 동작을 설명 -->

## 실제 동작

<!-- 실제 출력, 스택 트레이스, 어시션 실패 메시지 등 -->

## 추가 컨텍스트

- 발견된 Draco 예제 파일: `examples/coro_json.cpp`
- 해당 예제에서 호출한 beast_json API: `beast::parse()`, `Value::get()`, 등
````

### 리포팅 체크리스트

- [ ] 최소 재현 코드가 beast_json만으로 빌드·실행 가능한가?
- [ ] 컴파일러·OS 정보를 포함했는가?
- [ ] beast_json의 정확한 커밋 해시를 포함했는가?
- [ ] 기대 동작과 실제 동작을 명확히 구분했는가?
- [ ] 스택 트레이스 또는 오류 메시지 전문을 첨부했는가?

---

## 코드 기여

일반적인 PR 절차는 표준 GitHub Flow를 따릅니다.
브랜치명: `feat/<기능>`, `fix/<버그>`, `docs/<문서>`
