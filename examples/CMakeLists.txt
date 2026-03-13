add_executable(hello_world hello_world.cpp)
target_link_libraries(hello_world PRIVATE draco::draco)

add_executable(async_timer async_timer.cpp)
target_link_libraries(async_timer PRIVATE draco::draco)

# ── coro_json: 비동기 코루틴 + qbuem-json 예제 ────────────────────────────────
# qbuem-json 이 없을 경우 빌드를 건너뜀 (선택적 의존성).
FetchContent_Declare(qbuem_json
    GIT_REPOSITORY https://github.com/qbuem/qbuem-json.git
    GIT_TAG        ${QBUEM_JSON_TAG}
)
FetchContent_GetProperties(qbuem_json)
if(NOT qbuem_json_POPULATED)
    message(STATUS "Fetching qbuem-json (tag: ${QBUEM_JSON_TAG})")
    FetchContent_MakeAvailable(qbuem_json)
endif()

if(TARGET qbuem_json::qbuem_json)
    add_executable(coro_json coro_json.cpp)
    target_link_libraries(coro_json PRIVATE draco::draco qbuem_json::qbuem_json)
else()
    message(WARNING
        "qbuem-json target not found — skipping coro_json example.\n"
        "  Set -DQBUEM_JSON_TAG=<tag> or ensure network access."
    )
endif()
