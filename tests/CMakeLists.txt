# ============================================================================
# qbuem-stack unit tests
# ============================================================================

# ARM64 Bionic requires TLS segment alignment ≥ 64 bytes (PT_TLS p_align).
# tls_align_fix.cpp provides an alignas(64) thread_local that satisfies this.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  add_library(qbuem_tls_fix OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/tls_align_fix.cpp)
  set(QBUEM_TLS_FIX $<TARGET_OBJECTS:qbuem_tls_fix>)
else()
  set(QBUEM_TLS_FIX "")
endif()

# ── Core + HTTP + Pipeline tests ─────────────────────────────────────────────
add_executable(qbuem_tests
    # Layer 0-1: Core IO
    reactor_test.cpp
    dispatcher_post_test.cpp
    # Layer 2: HTTP
    http_test.cpp
    # Layer 6: Pipeline
    pipeline_test.cpp
    pipeline_integration_test.cpp
    arena_channel_test.cpp
    coroutine_context_test.cpp
    ${QBUEM_TLS_FIX}
)
target_link_libraries(qbuem_tests
    PRIVATE
    qbuem-stack::qbuem
    GTest::gtest_main
)

# ── Pipeline extensions + Resilience tests ───────────────────────────────────
# Separate executable to avoid test suite name collision with pipeline_integration_test.cpp
add_executable(qbuem_ext_tests
    static_pipeline_integration_test.cpp
    resilience_test.cpp
    ${QBUEM_TLS_FIX}
)
target_link_libraries(qbuem_ext_tests
    PRIVATE
    qbuem-stack::qbuem
    GTest::gtest_main
)

# ── v1.4.0: DB Abstraction + SHM Messaging tests ─────────────────────────────
add_executable(qbuem_v14_tests
    db_value_test.cpp
    shm_layout_test.cpp
    ${QBUEM_TLS_FIX}
)
target_link_libraries(qbuem_v14_tests
    PRIVATE
    qbuem-stack::qbuem
    GTest::gtest_main
)

# ── v1.5.0: Security & TLS tests ─────────────────────────────────────────────
add_executable(qbuem_v15_tests
    security_v15_test.cpp
    ${QBUEM_TLS_FIX}
)
target_link_libraries(qbuem_v15_tests
    PRIVATE
    qbuem-stack::qbuem
    GTest::gtest_main
)

# ── Crypto primitives: SHA, HMAC, Base64, PBKDF2, HKDF, ChaCha20-Poly1305, AES-GCM
add_executable(qbuem_crypto_tests
    crypto_primitives_test.cpp
    ${QBUEM_TLS_FIX}
)
target_link_libraries(qbuem_crypto_tests
    PRIVATE
    qbuem-stack::qbuem
    GTest::gtest_main
)

# ── LockFreeConnectionPool + FutexSync + JwtAuthAction tests ──────────────────
add_executable(qbuem_enhancement_tests
    enhancement_test.cpp
    ${QBUEM_TLS_FIX}
)
target_link_libraries(qbuem_enhancement_tests
    PRIVATE
    qbuem-stack::qbuem
    GTest::gtest_main
)

# ── Pipeline Guide: pattern & recipe integration tests ───────────────────────
add_executable(qbuem_pipeline_guide_tests
    pipeline_guide_test.cpp
    ${QBUEM_TLS_FIX}
)
target_link_libraries(qbuem_pipeline_guide_tests
    PRIVATE
    qbuem-stack::qbuem
    GTest::gtest_main
)

# ── URL / Codec / IO Buffer tests ─────────────────────────────────────────────
add_executable(qbuem_util_tests
    url_test.cpp
    codec_test.cpp
    io_buf_test.cpp
    ${QBUEM_TLS_FIX}
)
target_link_libraries(qbuem_util_tests
    PRIVATE
    qbuem-stack::qbuem
    GTest::gtest_main
)

# ── scattered_span + IOVec<N> tests (uses qbuem-stack::io, not umbrella) ──────
add_executable(qbuem_scattered_span_tests
    scattered_span_test.cpp
    ${QBUEM_TLS_FIX}
)
target_link_libraries(qbuem_scattered_span_tests
    PRIVATE
    qbuem-stack::io
    GTest::gtest_main
)
add_test(NAME qbuem_scattered_span_tests COMMAND qbuem_scattered_span_tests)

# ── Pipeline topology: Static / Dynamic(hot-swap+remove) / Graph(split+merge) ──
add_executable(qbuem_pipeline_topology_tests
    pipeline_topology_test.cpp
    ${QBUEM_TLS_FIX}
)
target_link_libraries(qbuem_pipeline_topology_tests
    PRIVATE
    qbuem-stack::qbuem
    GTest::gtest_main
)
add_test(NAME qbuem_pipeline_topology_tests COMMAND qbuem_pipeline_topology_tests)

# ── Per-module coverage suites (2026-06-15): config/tracing, codec, buf, net, io,
#    crypto-depth, pipeline-actions, middleware, core, db (~475 tests) ───────────
foreach(cov
    config_tracing  codec  buf  net  io
    crypto_depth  pipeline_actions  middleware  core  db)
    add_executable(qbuem_${cov}_cov_tests ${cov}_coverage_test.cpp ${QBUEM_TLS_FIX})
    target_link_libraries(qbuem_${cov}_cov_tests PRIVATE qbuem-stack::qbuem GTest::gtest_main)
    add_test(NAME qbuem_${cov}_cov_tests COMMAND qbuem_${cov}_cov_tests)
endforeach()

# ── Server + fetch + WebSocket end-to-end (loopback / socketpair / byte-level) ──
add_executable(qbuem_server_e2e_tests server_e2e_test.cpp ${QBUEM_TLS_FIX})
target_link_libraries(qbuem_server_e2e_tests PRIVATE qbuem-stack::qbuem GTest::gtest_main)
add_test(NAME qbuem_server_e2e_tests COMMAND qbuem_server_e2e_tests)

# ── High-level WsServer (handshake / framing / rooms / backpressure) ───────────
add_executable(qbuem_ws_server_tests ws_server_test.cpp ${QBUEM_TLS_FIX})
target_link_libraries(qbuem_ws_server_tests PRIVATE qbuem-stack::qbuem GTest::gtest_main)
add_test(NAME qbuem_ws_server_tests COMMAND qbuem_ws_server_tests)

# ── v1.2 high-level additions (secretbox / JWT / BufferedReader) ───────────────
add_executable(qbuem_v12_additions_tests v12_additions_test.cpp ${QBUEM_TLS_FIX})
target_link_libraries(qbuem_v12_additions_tests PRIVATE qbuem-stack::qbuem GTest::gtest_main)
add_test(NAME qbuem_v12_additions_tests COMMAND qbuem_v12_additions_tests)

# ── TickLoop (drift-free fixed-timestep + catch-up + metrics) ──────────────────
add_executable(qbuem_tick_loop_tests tick_loop_test.cpp ${QBUEM_TLS_FIX})
target_link_libraries(qbuem_tick_loop_tests PRIVATE qbuem-stack::qbuem GTest::gtest_main)
add_test(NAME qbuem_tick_loop_tests COMMAND qbuem_tick_loop_tests)

# ── App::ws() — WebSocket + HTTP on the same App/port (end-to-end) ─────────────
add_executable(qbuem_app_ws_tests app_ws_test.cpp ${QBUEM_TLS_FIX})
target_link_libraries(qbuem_app_ws_tests PRIVATE qbuem-stack::qbuem GTest::gtest_main)
add_test(NAME qbuem_app_ws_tests COMMAND qbuem_app_ws_tests)

# ── HTTP/2 SETTINGS negotiation + flow control (RFC 7540 §6.5/§6.9) ────────────
add_executable(qbuem_http2_flow_control_tests http2_flow_control_test.cpp ${QBUEM_TLS_FIX})
target_link_libraries(qbuem_http2_flow_control_tests PRIVATE qbuem-stack::qbuem GTest::gtest_main)
add_test(NAME qbuem_http2_flow_control_tests COMMAND qbuem_http2_flow_control_tests)

# ── RS256 (RSASSA-PKCS1-v1.5 + SHA-256) signature verification ─────────────────
add_executable(qbuem_rsa_verify_tests rsa_verify_test.cpp ${QBUEM_TLS_FIX})
target_link_libraries(qbuem_rsa_verify_tests PRIVATE qbuem-stack::qbuem GTest::gtest_main)
add_test(NAME qbuem_rsa_verify_tests COMMAND qbuem_rsa_verify_tests)

# ── RS256 JWT + JWKS verifier (external-IdP token auth, end-to-end) ────────────
add_executable(qbuem_rs256_jwt_tests rs256_jwt_test.cpp ${QBUEM_TLS_FIX})
target_link_libraries(qbuem_rs256_jwt_tests PRIVATE qbuem-stack::qbuem GTest::gtest_main)
add_test(NAME qbuem_rs256_jwt_tests COMMAND qbuem_rs256_jwt_tests)

# ── Per-tenant quota middleware (SaaS multi-tenancy) ───────────────────────────
add_executable(qbuem_quota_tests quota_test.cpp ${QBUEM_TLS_FIX})
target_link_libraries(qbuem_quota_tests PRIVATE qbuem-stack::qbuem GTest::gtest_main)
add_test(NAME qbuem_quota_tests COMMAND qbuem_quota_tests)

# ── OTLP/JSON span exporter (off-process tracing) ──────────────────────────────
add_executable(qbuem_otlp_exporter_tests otlp_exporter_test.cpp ${QBUEM_TLS_FIX})
target_link_libraries(qbuem_otlp_exporter_tests PRIVATE qbuem-stack::qbuem GTest::gtest_main)
add_test(NAME qbuem_otlp_exporter_tests COMMAND qbuem_otlp_exporter_tests)

# ── OffloadPool + Dispatcher graceful drain (thread-management) ────────────────
add_executable(qbuem_offload_drain_tests offload_drain_test.cpp ${QBUEM_TLS_FIX})
target_link_libraries(qbuem_offload_drain_tests PRIVATE qbuem-stack::qbuem GTest::gtest_main)
add_test(NAME qbuem_offload_drain_tests COMMAND qbuem_offload_drain_tests)

# ── Action-wrapper teardown safety (destroy while workers in flight) ───────────
add_executable(qbuem_action_lifetime_tests action_lifetime_test.cpp ${QBUEM_TLS_FIX})
target_link_libraries(qbuem_action_lifetime_tests PRIVATE qbuem-stack::qbuem GTest::gtest_main)
add_test(NAME qbuem_action_lifetime_tests COMMAND qbuem_action_lifetime_tests)

# ── Tracing / Timer / Arena / AsyncLogger / SocketAddr tests ──────────────────
add_executable(qbuem_infra_tests
    tracing_test.cpp
    timer_wheel_test.cpp
    arena_test.cpp
    async_logger_test.cpp
    socket_addr_test.cpp
    ${QBUEM_TLS_FIX}
)
target_link_libraries(qbuem_infra_tests
    PRIVATE
    qbuem-stack::qbuem
    GTest::gtest_main
)

# ── HTTP Middleware tests ──────────────────────────────────────────────────────
add_executable(qbuem_middleware_tests
    http_middleware_test.cpp
    ${QBUEM_TLS_FIX}
)
target_link_libraries(qbuem_middleware_tests
    PRIVATE
    qbuem-stack::qbuem
    GTest::gtest_main
)

# ── Advanced Pipeline: Observability / Health / Saga / Checkpoint / Idempotency
add_executable(qbuem_pipeline_advanced_tests
    pipeline_advanced_test.cpp
    ${QBUEM_TLS_FIX}
)
target_link_libraries(qbuem_pipeline_advanced_tests
    PRIVATE
    qbuem-stack::qbuem
    GTest::gtest_main
)

# ── SHMChannel + SHMBus functional tests ──────────────────────────────────────
add_executable(qbuem_shm_tests
    shm_channel_test.cpp
    ${QBUEM_TLS_FIX}
)
target_link_libraries(qbuem_shm_tests
    PRIVATE
    qbuem-stack::qbuem
    GTest::gtest_main
)

# ── CanaryRouter + ScatterGatherAction / DebounceAction / ThrottleAction ──────
add_executable(qbuem_canary_scatter_tests
    canary_test.cpp
    scatter_gather_test.cpp
    ${QBUEM_TLS_FIX}
)
target_link_libraries(qbuem_canary_scatter_tests
    PRIVATE
    qbuem-stack::qbuem
    GTest::gtest_main
)

# ── Pipeline <-> MessageBus IPC integration tests (with_source/with_sink) ─────
add_executable(qbuem_pipeline_ipc_tests
    pipeline_ipc_test.cpp
    ${QBUEM_TLS_FIX}
)
target_link_libraries(qbuem_pipeline_ipc_tests
    PRIVATE
    qbuem-stack::qbuem
    GTest::gtest_main
)

# ── Kqueue Enhancement tests (macOS only) ────────────────────────────────────
if(APPLE)
add_executable(qbuem_kqueue_tests
    kqueue_test.cpp
)
target_link_libraries(qbuem_kqueue_tests
    PRIVATE
    qbuem-stack::qbuem
    GTest::gtest_main
)
endif()

# ── Expanded coverage (2026-06-14): net loopback, JWT verify+tamper, router,
#    buf pools (GenerationPool/LockFreeHashMap/simd_erasure), io buffers,
#    content-type/body-encoder middleware. Authored + self-verified. ──────────
add_executable(qbuem_coverage_tests
    net_loopback_test.cpp
    jwt_verify_test.cpp
    router_test.cpp
    buf_pools_test.cpp
    io_buffers_test.cpp
    middleware_extra_test.cpp
    inplace_function_test.cpp
    ${QBUEM_TLS_FIX}
)
target_link_libraries(qbuem_coverage_tests
    PRIVATE
    qbuem-stack::qbuem
    GTest::gtest_main
)
# router_test.cpp inline-#includes src/http/*.cpp for standalone self-verify;
# define this so it links the library instead (avoids duplicate symbols).
target_compile_definitions(qbuem_coverage_tests PRIVATE QBUEM_ROUTER_TEST_LINK_LIB)
add_test(NAME qbuem_coverage_tests COMMAND qbuem_coverage_tests)

# reactor_loop_test inline-#includes src/core/kqueue_reactor.cpp (real macOS
# kqueue event-loop behavior) → macOS-only; links only gtest (no reactor lib).
if(APPLE)
  add_executable(qbuem_reactor_loop_tests reactor_loop_test.cpp ${QBUEM_TLS_FIX})
  target_include_directories(qbuem_reactor_loop_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
  target_link_libraries(qbuem_reactor_loop_tests PRIVATE GTest::gtest_main)
  add_test(NAME qbuem_reactor_loop_tests COMMAND qbuem_reactor_loop_tests)
endif()

# Use add_test (not gtest_discover_tests) to avoid running binaries at build
# time.  On ARM64 Bionic (Termux) some executables have TLS alignment issues
# that cause the dynamic linker to abort them; deferring to ctest keeps the
# build clean and lets individual tests be skipped at run time.
add_test(NAME qbuem_shm_tests              COMMAND qbuem_shm_tests)
add_test(NAME qbuem_canary_scatter_tests   COMMAND qbuem_canary_scatter_tests)
add_test(NAME qbuem_tests                  COMMAND qbuem_tests)
add_test(NAME qbuem_ext_tests              COMMAND qbuem_ext_tests)
add_test(NAME qbuem_v14_tests              COMMAND qbuem_v14_tests)
add_test(NAME qbuem_v15_tests              COMMAND qbuem_v15_tests)
add_test(NAME qbuem_crypto_tests           COMMAND qbuem_crypto_tests)
add_test(NAME qbuem_enhancement_tests      COMMAND qbuem_enhancement_tests)
add_test(NAME qbuem_pipeline_guide_tests   COMMAND qbuem_pipeline_guide_tests)
add_test(NAME qbuem_util_tests             COMMAND qbuem_util_tests)
add_test(NAME qbuem_infra_tests            COMMAND qbuem_infra_tests)
add_test(NAME qbuem_middleware_tests       COMMAND qbuem_middleware_tests)
add_test(NAME qbuem_pipeline_advanced_tests COMMAND qbuem_pipeline_advanced_tests)
add_test(NAME qbuem_pipeline_ipc_tests     COMMAND qbuem_pipeline_ipc_tests)

# ── GridBitset radius queries + TiledBitset tests ─────────────────────────────
add_executable(qbuem_grid_spatial_tests
    grid_spatial_test.cpp
    ${QBUEM_TLS_FIX}
)
target_link_libraries(qbuem_grid_spatial_tests
    PRIVATE
    qbuem-stack::qbuem
    GTest::gtest_main
)
add_test(NAME qbuem_grid_spatial_tests COMMAND qbuem_grid_spatial_tests)
if(APPLE)
add_test(NAME qbuem_kqueue_tests           COMMAND qbuem_kqueue_tests)
endif()

# SIMD correctness validation (standalone, no umbrella library)
# Only build on ARM64 where NEON is available
if(QBUEM_BUILD_TESTS AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  add_executable(qbuem_neon_validation
      ${CMAKE_CURRENT_SOURCE_DIR}/neon_validation.cpp
      ${QBUEM_TLS_FIX}
  )
  target_include_directories(qbuem_neon_validation PRIVATE
      ${PROJECT_SOURCE_DIR}/include)
  target_compile_options(qbuem_neon_validation PRIVATE -march=native)
  add_test(NAME neon_validation COMMAND qbuem_neon_validation)
endif()
