# qbuem-stack — Contributing Guide

## JSON 라이브러리 정책

Draco WAS 프레임워크 **코어는 JSON 라이브러리에 의존하지 않습니다.**

| 계층 | JSON 의존성 |
|------|------------|
| `draco::core` | ❌ 없음 |
| `draco::http` | ❌ 없음 — `body()`는 `std::string_view` raw bytes |
| `draco::draco` | ❌ 없음 |

애플리케이션에서는 simdjson, nlohmann/json, glaze 등 원하는 라이브러리를 자유롭게
선택하여 `req.body()`를 파싱하면 됩니다.

---

## 코드 기여

일반적인 PR 절차는 표준 GitHub Flow를 따릅니다.
브랜치명: `feat/<기능>`, `fix/<버그>`, `docs/<문서>`
