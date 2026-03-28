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

# ── v1.6.0: PCIe/VFIO + MSI-X + UDS Advanced tests ──────────────────────────
add_executable(qbuem_v16_tests
    pcie_layout_test.cpp
    ${QBUEM_TLS_FIX}
)
target_link_libraries(qbuem_v16_tests
    PRIVATE
    qbuem-stack::qbuem
    GTest::gtest_main
)

# ── v1.7.0: RDMA + eBPF + NVMe IO tests ──────────────────────────────────────
add_executable(qbuem_v17_tests
    spdk_layout_test.cpp
    ${QBUEM_TLS_FIX}
)
target_link_libraries(qbuem_v17_tests
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
add_test(NAME qbuem_v16_tests              COMMAND qbuem_v16_tests)
add_test(NAME qbuem_v17_tests              COMMAND qbuem_v17_tests)
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
