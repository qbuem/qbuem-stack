# ============================================================================
# qbuem-stack unit tests
# ============================================================================

# ── Core + HTTP tests (zero external dependency) ─────────────────────────────
add_executable(qbuem_tests
    reactor_test.cpp
    http_test.cpp
    dispatcher_post_test.cpp
    pipeline_test.cpp
    pipeline_integration_test.cpp
    arena_channel_test.cpp
)
target_link_libraries(qbuem_tests
    PRIVATE
    qbuem-stack::qbuem
    GTest::gtest_main
)
include(GoogleTest)
gtest_discover_tests(qbuem_tests)
