add_executable(hello_world hello_world.cpp)
target_link_libraries(hello_world PRIVATE qbuem-stack::qbuem)

add_executable(async_timer async_timer.cpp)
target_link_libraries(async_timer PRIVATE qbuem-stack::qbuem)

add_executable(kqueue_sophistication kqueue_sophistication.cpp)
target_link_libraries(kqueue_sophistication PRIVATE qbuem-stack::qbuem)

# ── Pipeline Guide 예제 ───────────────────────────────────────────────────────

# §5-1: Fan-out (Broadcast) / Fan-in (Merge) — PipelineGraph 1→N / N→1
add_executable(pipeline_fanout pipeline_fanout.cpp)
target_link_libraries(pipeline_fanout PRIVATE qbuem-stack::qbuem)

# §6A Recipe A: Sensor Fusion — ServiceRegistry N:1 Sync Gather
add_executable(pipeline_sensor_fusion pipeline_sensor_fusion.cpp)
target_link_libraries(pipeline_sensor_fusion PRIVATE qbuem-stack::qbuem)

# §6B Recipe B: Hardware Batching (NPU) — BatchAction
add_executable(pipeline_hardware_batching pipeline_hardware_batching.cpp)
target_link_libraries(pipeline_hardware_batching PRIVATE qbuem-stack::qbuem)

# §3-2 + §6C: DynamicPipeline Hot-swap + DLQ
add_executable(pipeline_dynamic_hotswap pipeline_dynamic_hotswap.cpp)
target_link_libraries(pipeline_dynamic_hotswap PRIVATE qbuem-stack::qbuem)

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
    target_link_libraries(coro_json PRIVATE qbuem-stack::qbuem qbuem_json::qbuem_json)
else()
    message(WARNING
        "qbuem-json target not found — skipping coro_json example.\n"
        "  Set -DQBUEM_JSON_TAG=<tag> or ensure network access."
    )
endif()
