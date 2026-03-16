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

# RetryAction + CircuitBreaker + DeadLetterQueue: 결제 게이트웨이 복원력 패턴
add_executable(resilience_example resilience_example.cpp)
target_link_libraries(resilience_example PRIVATE qbuem-stack::qbuem)

# SagaOrchestrator: 분산 트랜잭션 (이커머스 주문 처리)
add_executable(saga_example saga_example.cpp)
target_link_libraries(saga_example PRIVATE qbuem-stack::qbuem)

# CheckpointedPipeline: 배치 로그 처리기 + 체크포인트 저장/복원
add_executable(checkpoint_example checkpoint_example.cpp)
target_link_libraries(checkpoint_example PRIVATE qbuem-stack::qbuem)

# IdempotencyFilter + ErrorBudgetTracker + LatencyHistogram: 결제 중복제거 + SLO
add_executable(idempotency_slo_example idempotency_slo_example.cpp)
target_link_libraries(idempotency_slo_example PRIVATE qbuem-stack::qbuem)

# TaskGroup: 구조적 동시성 (병렬 서비스 조회, 팬아웃, 취소)
add_executable(task_group_example task_group_example.cpp)
target_link_libraries(task_group_example PRIVATE qbuem-stack::qbuem)

# IPC Pipeline: SHMSource/SHMSink + MessageBusSource/MessageBusSink + with_source/with_sink
add_executable(ipc_pipeline_example ipc_pipeline_example.cpp)
target_link_libraries(ipc_pipeline_example PRIVATE qbuem-stack::qbuem)

# ── 신규 기능 예제 ────────────────────────────────────────────────────────────

# UDP + Unix Domain Socket: UdpSocket, UnixSocket, SocketAddr
add_executable(udp_unix_socket_example udp_unix_socket_example.cpp)
target_link_libraries(udp_unix_socket_example PRIVATE qbuem-stack::qbuem)

# NUMA + Huge Pages + CPU Hints + I/O Slice: pin_reactor_to_cpu, HugeBufferPool, prefetch_*
add_executable(numa_hugepages_example numa_hugepages_example.cpp)
target_link_libraries(numa_hugepages_example PRIVATE qbuem-stack::qbuem)

# WindowedAction: TumblingWindow, SlidingWindow, SessionWindow, Watermark, EventTime
add_executable(windowed_action_example windowed_action_example.cpp)
target_link_libraries(windowed_action_example PRIVATE qbuem-stack::qbuem)

# PipelineObserver + PipelineHealth: ActionMetrics, PipelineVersion, to_json/dot/mermaid
add_executable(pipeline_observer_health_example pipeline_observer_health_example.cpp)
target_link_libraries(pipeline_observer_health_example PRIVATE qbuem-stack::qbuem)

# SubpipelineAction + MigrationAction + DlqReprocessor: 타입 마이그레이션
add_executable(subpipeline_migration_example subpipeline_migration_example.cpp)
target_link_libraries(subpipeline_migration_example PRIVATE qbuem-stack::qbuem)

# PipelineFactory: JSON 기반 DynamicPipeline / PipelineGraph 조립
add_executable(pipeline_factory_example pipeline_factory_example.cpp)
target_link_libraries(pipeline_factory_example PRIVATE qbuem-stack::qbuem)

# PriorityChannel + SpscChannel: 우선순위 채널, SPSC 락-프리 채널
add_executable(priority_spsc_channel_example priority_spsc_channel_example.cpp)
target_link_libraries(priority_spsc_channel_example PRIVATE qbuem-stack::qbuem)

# Crypto + URL: constant_time_eq, CSPRNG, CSRF 토큰, url_encode/decode
add_executable(crypto_url_example crypto_url_example.cpp)
target_link_libraries(crypto_url_example PRIVATE qbuem-stack::qbuem)

# 보안 미들웨어: security.hpp, token_auth, static_files, body_encoder
add_executable(security_middleware_example security_middleware_example.cpp)
target_link_libraries(security_middleware_example PRIVATE qbuem-stack::qbuem)

# DB 연결 풀 + 세션 저장소: LockFreeConnectionPool, InMemorySessionStore
add_executable(db_session_example db_session_example.cpp)
target_link_libraries(db_session_example PRIVATE qbuem-stack::qbuem)

# 하드웨어 I/O: PCIeDevice, RDMAChannel, EBPFTracer, NVMeIO, kTLS
add_executable(hardware_io_example hardware_io_example.cpp)
target_link_libraries(hardware_io_example PRIVATE qbuem-stack::qbuem)

# Zero-copy + ArenaChannel + AsyncLogger: sendfile, splice, arena_channel
add_executable(zero_copy_arena_channel_example zero_copy_arena_channel_example.cpp)
target_link_libraries(zero_copy_arena_channel_example PRIVATE qbuem-stack::qbuem)

# ── trading_platform: 종합 예제 ──────────────────────────────────────────────
# WAS + StaticPipeline(3 Actions, auto_scale, SLO) + DynamicPipeline(hot_swap, toggle)
# + MessageBus(SSE fan-out) + MockDB + JSON + Context 전파 + Cross-reactor 응답 채널
add_executable(trading_platform trading_platform.cpp)
target_link_libraries(trading_platform PRIVATE qbuem-stack::qbuem)

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
