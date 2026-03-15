# ============================================================================
# qbuem-stack unit tests
# ============================================================================

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
)
target_link_libraries(qbuem_tests
    PRIVATE
    qbuem-stack::qbuem
    GTest::gtest_main
)

# ── Pipeline extensions + Resilience tests ───────────────────────────────────
# 별도 실행 파일: pipeline_integration_test.cpp과 test suite 이름 충돌 방지
add_executable(qbuem_ext_tests
    static_pipeline_integration_test.cpp
    resilience_test.cpp
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
)
target_link_libraries(qbuem_v14_tests
    PRIVATE
    qbuem-stack::qbuem
    GTest::gtest_main
)

# ── v1.5.0: Security & TLS tests ─────────────────────────────────────────────
add_executable(qbuem_v15_tests
    security_v15_test.cpp
)
target_link_libraries(qbuem_v15_tests
    PRIVATE
    qbuem-stack::qbuem
    GTest::gtest_main
)

# ── v1.6.0: PCIe/VFIO + MSI-X + UDS Advanced tests ──────────────────────────
add_executable(qbuem_v16_tests
    pcie_layout_test.cpp
)
target_link_libraries(qbuem_v16_tests
    PRIVATE
    qbuem-stack::qbuem
    GTest::gtest_main
)

# ── v1.7.0: RDMA + eBPF + NVMe IO tests ──────────────────────────────────────
add_executable(qbuem_v17_tests
    spdk_layout_test.cpp
)
target_link_libraries(qbuem_v17_tests
    PRIVATE
    qbuem-stack::qbuem
    GTest::gtest_main
)

# ── 고도화: LockFreeConnectionPool + FutexSync + JwtAuthAction tests ──────────
add_executable(qbuem_enhancement_tests
    enhancement_test.cpp
)
target_link_libraries(qbuem_enhancement_tests
    PRIVATE
    qbuem-stack::qbuem
    GTest::gtest_main
)

# ── Pipeline Guide: 패턴 & 레시피 통합 테스트 ────────────────────────────────
add_executable(qbuem_pipeline_guide_tests
    pipeline_guide_test.cpp
)
target_link_libraries(qbuem_pipeline_guide_tests
    PRIVATE
    qbuem-stack::qbuem
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(qbuem_tests)
gtest_discover_tests(qbuem_ext_tests)
gtest_discover_tests(qbuem_v14_tests)
gtest_discover_tests(qbuem_v15_tests)
gtest_discover_tests(qbuem_v16_tests)
gtest_discover_tests(qbuem_v17_tests)
gtest_discover_tests(qbuem_enhancement_tests)
gtest_discover_tests(qbuem_pipeline_guide_tests)
