# ==============================================================================
# qbuem-stack Examples — CMakeLists.txt
#
# Examples are organized into category subdirectories.
# Each subdirectory contains a source file and a README.md.
#
# Category layout:
#   01-foundation/        hello_world, async_timer
#   02-network/           tcp_echo_server, udp_unix_socket, websocket
#   03-memory/            arena, zero_copy_arena_channel, numa_hugepages
#   04-codec-security/    codec, crypto_url, security_middleware
#   05-pipeline/          fanout, hardware_batching, dynamic_hotswap,
#                         sensor_fusion, observer_health, factory (json),
#                         subpipeline_migration, stream_ops, windowed_action
#   06-ipc-messaging/     shm_channel, ipc_pipeline, message_bus,
#                         priority_spsc_channel
#   07-resilience/        canary, checkpoint, resilience, saga,
#                         scatter_gather, idempotency_slo
#   08-observability/     tracing, timer_wheel, task_group
#   09-database/          db_session (json), coro_json
#   10-hardware/          hardware_io, kqueue_sophistication (Apple)
#   11-advanced-apps/     autonomous_driving, sensor_fusion,
#                         io_metrics_dashboard, trading_platform (json),
#                         game_server (json), middleware (json)
# ==============================================================================

# ── 01 Foundation ─────────────────────────────────────────────────────────────

add_executable(hello_world
    01-foundation/hello_world/hello_world.cpp)
target_link_libraries(hello_world PRIVATE qbuem-stack::qbuem)

add_executable(async_timer
    01-foundation/async_timer/async_timer.cpp)
target_link_libraries(async_timer PRIVATE qbuem-stack::qbuem)

# ── 02 Network ────────────────────────────────────────────────────────────────

add_executable(tcp_echo_server
    02-network/tcp_echo_server/tcp_echo_server.cpp)
target_link_libraries(tcp_echo_server PRIVATE qbuem-stack::qbuem)

add_executable(udp_unix_socket_example
    02-network/udp_unix_socket/udp_unix_socket_example.cpp)
target_link_libraries(udp_unix_socket_example PRIVATE qbuem-stack::qbuem)

add_executable(websocket_example
    02-network/websocket/websocket_example.cpp)
target_link_libraries(websocket_example PRIVATE qbuem-stack::qbuem)

# ── 03 Memory ─────────────────────────────────────────────────────────────────

add_executable(arena_example
    03-memory/arena/arena_example.cpp)
target_link_libraries(arena_example PRIVATE qbuem-stack::qbuem)

add_executable(zero_copy_arena_channel_example
    03-memory/zero_copy_arena_channel/zero_copy_arena_channel_example.cpp)
target_link_libraries(zero_copy_arena_channel_example PRIVATE qbuem-stack::qbuem)

add_executable(numa_hugepages_example
    03-memory/numa_hugepages/numa_hugepages_example.cpp)
target_link_libraries(numa_hugepages_example PRIVATE qbuem-stack::qbuem)

# ── 04 Codec & Security ───────────────────────────────────────────────────────

add_executable(codec_example
    04-codec-security/codec/codec_example.cpp)
target_link_libraries(codec_example PRIVATE qbuem-stack::qbuem)

add_executable(crypto_url_example
    04-codec-security/crypto_url/crypto_url_example.cpp)
target_link_libraries(crypto_url_example PRIVATE qbuem-stack::qbuem)

add_executable(security_middleware_example
    04-codec-security/security_middleware/security_middleware_example.cpp)
target_link_libraries(security_middleware_example PRIVATE qbuem-stack::qbuem)

# ── 05 Pipeline ───────────────────────────────────────────────────────────────

# §5-1: Fan-out (Broadcast) / Fan-in (Merge) — PipelineGraph 1→N / N→1
add_executable(pipeline_fanout
    05-pipeline/fanout/pipeline_fanout.cpp)
target_link_libraries(pipeline_fanout PRIVATE qbuem-stack::qbuem)

# §6A Recipe A: Sensor Fusion — ServiceRegistry N:1 Sync Gather
add_executable(pipeline_sensor_fusion
    05-pipeline/sensor_fusion/pipeline_sensor_fusion.cpp)
target_link_libraries(pipeline_sensor_fusion PRIVATE qbuem-stack::qbuem)

# §6B Recipe B: Hardware Batching (NPU) — BatchAction
add_executable(pipeline_hardware_batching
    05-pipeline/hardware_batching/pipeline_hardware_batching.cpp)
target_link_libraries(pipeline_hardware_batching PRIVATE qbuem-stack::qbuem)

# §3-2 + §6C: DynamicPipeline Hot-swap + DLQ
add_executable(pipeline_dynamic_hotswap
    05-pipeline/dynamic_hotswap/pipeline_dynamic_hotswap.cpp)
target_link_libraries(pipeline_dynamic_hotswap PRIVATE qbuem-stack::qbuem)

# PipelineObserver + PipelineHealth: ActionMetrics, PipelineVersion
add_executable(pipeline_observer_health_example
    05-pipeline/observer_health/pipeline_observer_health_example.cpp)
target_link_libraries(pipeline_observer_health_example PRIVATE qbuem-stack::qbuem)

# SubpipelineAction + MigrationAction + DlqReprocessor
add_executable(subpipeline_migration_example
    05-pipeline/subpipeline_migration/subpipeline_migration_example.cpp)
target_link_libraries(subpipeline_migration_example PRIVATE qbuem-stack::qbuem)

# Stream operators: map, filter, throttle, debounce, tumbling_window
add_executable(stream_ops_example
    05-pipeline/stream_ops/stream_ops_example.cpp)
target_link_libraries(stream_ops_example PRIVATE qbuem-stack::qbuem)

# WindowedAction: TumblingWindow, SlidingWindow, SessionWindow, Watermark
add_executable(windowed_action_example
    05-pipeline/windowed_action/windowed_action_example.cpp)
target_link_libraries(windowed_action_example PRIVATE qbuem-stack::qbuem)

# ── 06 IPC & Messaging ────────────────────────────────────────────────────────

# SHMChannel + SHMBus: shared memory channel + bus
add_executable(shm_channel_example
    06-ipc-messaging/shm_channel/shm_channel_example.cpp)
target_link_libraries(shm_channel_example PRIVATE qbuem-stack::qbuem)

# IPC Pipeline: SHMSource/SHMSink + MessageBusSource/MessageBusSink
add_executable(ipc_pipeline_example
    06-ipc-messaging/ipc_pipeline/ipc_pipeline_example.cpp)
target_link_libraries(ipc_pipeline_example PRIVATE qbuem-stack::qbuem)

# MessageBus: Pub/Sub subscribe, publish, try_publish
add_executable(message_bus_example
    06-ipc-messaging/message_bus/message_bus_example.cpp)
target_link_libraries(message_bus_example PRIVATE qbuem-stack::qbuem)

# PriorityChannel + SpscChannel
add_executable(priority_spsc_channel_example
    06-ipc-messaging/priority_spsc_channel/priority_spsc_channel_example.cpp)
target_link_libraries(priority_spsc_channel_example PRIVATE qbuem-stack::qbuem)

# ── 07 Resilience ─────────────────────────────────────────────────────────────

# CanaryRouter: progressive canary deployment + metrics
add_executable(canary_example
    07-resilience/canary/canary_example.cpp)
target_link_libraries(canary_example PRIVATE qbuem-stack::qbuem)

# CheckpointedPipeline: batch log processor + checkpoint save/restore
add_executable(checkpoint_example
    07-resilience/checkpoint/checkpoint_example.cpp)
target_link_libraries(checkpoint_example PRIVATE qbuem-stack::qbuem)

# RetryAction + CircuitBreaker + DeadLetterQueue: payment gateway resilience
add_executable(resilience_example
    07-resilience/resilience/resilience_example.cpp)
target_link_libraries(resilience_example PRIVATE qbuem-stack::qbuem)

# SagaOrchestrator: distributed transactions (e-commerce order processing)
add_executable(saga_example
    07-resilience/saga/saga_example.cpp)
target_link_libraries(saga_example PRIVATE qbuem-stack::qbuem)

# ScatterGatherAction / DebounceAction / ThrottleAction
add_executable(scatter_gather_example
    07-resilience/scatter_gather/scatter_gather_example.cpp)
target_link_libraries(scatter_gather_example PRIVATE qbuem-stack::qbuem)

# IdempotencyFilter + ErrorBudgetTracker + LatencyHistogram: SLO tracking
add_executable(idempotency_slo_example
    07-resilience/idempotency_slo/idempotency_slo_example.cpp)
target_link_libraries(idempotency_slo_example PRIVATE qbuem-stack::qbuem)

# ── 08 Observability ─────────────────────────────────────────────────────────

# Tracing: W3C TraceContext, PipelineTracer, Samplers, SpanExporter
add_executable(tracing_example
    08-observability/tracing/tracing_example.cpp)
target_link_libraries(tracing_example PRIVATE qbuem-stack::qbuem)

# TimerWheel: schedule, cancel, tick, next_expiry_ms
add_executable(timer_wheel_example
    08-observability/timer_wheel/timer_wheel_example.cpp)
target_link_libraries(timer_wheel_example PRIVATE qbuem-stack::qbuem)

# TaskGroup: structured concurrency (parallel service queries, fan-out, cancel)
add_executable(task_group_example
    08-observability/task_group/task_group_example.cpp)
target_link_libraries(task_group_example PRIVATE qbuem-stack::qbuem)

# ── 10 Hardware ───────────────────────────────────────────────────────────────

# Hardware I/O: PCIeDevice, RDMAChannel, EBPFTracer, NVMeIO, kTLS
add_executable(hardware_io_example
    10-hardware/hardware_io/hardware_io_example.cpp)
target_link_libraries(hardware_io_example PRIVATE qbuem-stack::qbuem)

if(APPLE)
    add_executable(kqueue_sophistication
        10-hardware/kqueue_sophistication/kqueue_sophistication.cpp)
    target_link_libraries(kqueue_sophistication PRIVATE qbuem-stack::qbuem)
endif()

# ── 11 Advanced Applications ─────────────────────────────────────────────────

# Sensor Fusion: Mock IMU + GPS + LiDAR → StaticPipeline → DynamicPipeline
add_executable(sensor_fusion_example
    11-advanced-apps/sensor_fusion/sensor_fusion_example.cpp)
target_link_libraries(sensor_fusion_example PRIVATE qbuem-stack::qbuem)

# Autonomous Driving: 6-sensor EKF+SORT fusion + AEB hot-swap + MessageBus
add_executable(autonomous_driving_fusion
    11-advanced-apps/autonomous_driving/autonomous_driving_fusion.cpp)
target_link_libraries(autonomous_driving_fusion PRIVATE qbuem-stack::qbuem)

# I/O Metrics Dashboard: 4 producers × 20k events → p50/p95/p99/p999
add_executable(io_metrics_dashboard
    11-advanced-apps/io_metrics_dashboard/io_metrics_dashboard.cpp)
target_link_libraries(io_metrics_dashboard PRIVATE qbuem-stack::qbuem)

# ── qbuem-json dependent examples ────────────────────────────────────────────
# Fetches qbuem-json via FetchContent; skipped if unavailable or no network.

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
    # 05 Pipeline — Factory (JSON-driven pipeline builder)
    add_executable(pipeline_factory_example
        05-pipeline/factory/pipeline_factory_example.cpp)
    target_link_libraries(pipeline_factory_example
        PRIVATE qbuem-stack::qbuem qbuem_json::qbuem_json)

    # 09 Database — Connection pool + session store
    add_executable(db_session_example
        09-database/db_session/db_session_example.cpp)
    target_link_libraries(db_session_example
        PRIVATE qbuem-stack::qbuem qbuem_json::qbuem_json)

    # 09 Database — Coroutine JSON request/response
    add_executable(coro_json
        09-database/coro_json/coro_json.cpp)
    target_link_libraries(coro_json
        PRIVATE qbuem-stack::qbuem qbuem_json::qbuem_json)

    # 11 Advanced — HTTP middleware: CORS, RateLimit, RequestID, HSTS, BearerAuth, SSE
    add_executable(middleware_example
        11-advanced-apps/middleware/middleware_example.cpp)
    target_link_libraries(middleware_example
        PRIVATE qbuem-stack::qbuem qbuem_json::qbuem_json)

    # 11 Advanced — Full HFT trading platform
    add_executable(trading_platform
        11-advanced-apps/trading_platform/trading_platform.cpp)
    target_link_libraries(trading_platform
        PRIVATE qbuem-stack::qbuem qbuem_json::qbuem_json)

    # 11 Advanced — Real-time multiplayer turn-based game server
    add_executable(game_server
        11-advanced-apps/game_server/game_server.cpp)
    target_link_libraries(game_server
        PRIVATE qbuem-stack::qbuem qbuem_json::qbuem_json)
else()
    message(WARNING
        "qbuem-json target not found — skipping:\n"
        "  pipeline_factory_example, db_session_example, coro_json,\n"
        "  middleware_example, trading_platform, game_server\n"
        "  Set -DQBUEM_JSON_TAG=<tag> or ensure network access."
    )
endif()
