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

# ── Feature examples ─────────────────────────────────────────────────────────

# Middleware: CORS, RateLimit, RequestID, HSTS, BearerAuth, SSE
add_executable(middleware_example middleware_example.cpp)
target_link_libraries(middleware_example PRIVATE qbuem-stack::qbuem)

# WebSocket: RFC 6455 frame encode/decode, compute_accept_key
add_executable(websocket_example websocket_example.cpp)
target_link_libraries(websocket_example PRIVATE qbuem-stack::qbuem)

# Codec: LengthPrefixedCodec, LineCodec
add_executable(codec_example codec_example.cpp)
target_link_libraries(codec_example PRIVATE qbuem-stack::qbuem)

# Tracing: W3C TraceContext, PipelineTracer, Samplers, SpanExporter
add_executable(tracing_example tracing_example.cpp)
target_link_libraries(tracing_example PRIVATE qbuem-stack::qbuem)

# TimerWheel: schedule, cancel, tick, next_expiry_ms
add_executable(timer_wheel_example timer_wheel_example.cpp)
target_link_libraries(timer_wheel_example PRIVATE qbuem-stack::qbuem)

# Arena + FixedPoolResource + AsyncLogger
add_executable(arena_example arena_example.cpp)
target_link_libraries(arena_example PRIVATE qbuem-stack::qbuem)

# MessageBus: Pub/Sub subscribe, publish, try_publish
add_executable(message_bus_example message_bus_example.cpp)
target_link_libraries(message_bus_example PRIVATE qbuem-stack::qbuem)

# Stream operators: map, filter, throttle, debounce, tumbling_window
add_executable(stream_ops_example stream_ops_example.cpp)
target_link_libraries(stream_ops_example PRIVATE qbuem-stack::qbuem)

# TCP Echo Server: TcpListener, TcpStream, accept loop
add_executable(tcp_echo_server tcp_echo_server.cpp)
target_link_libraries(tcp_echo_server PRIVATE qbuem-stack::qbuem)

# SHMChannel + SHMBus: 공유 메모리 채널 create/send/recv, 버스 declare/publish/subscribe
add_executable(shm_channel_example shm_channel_example.cpp)
target_link_libraries(shm_channel_example PRIVATE qbuem-stack::qbuem)

# CanaryRouter: 점진적 카나리 배포, 지표 수집, 자동/수동 롤백
add_executable(canary_example canary_example.cpp)
target_link_libraries(canary_example PRIVATE qbuem-stack::qbuem)

# ScatterGatherAction: 분산/병렬/집계 파이프라인, max_parallel 배치 제한
add_executable(scatter_gather_example scatter_gather_example.cpp)
target_link_libraries(scatter_gather_example PRIVATE qbuem-stack::qbuem)

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
