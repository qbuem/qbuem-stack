# ============================================================================
# qbuem-stack unit tests
# ============================================================================

# ── Core + HTTP tests (zero external dependency) ─────────────────────────────
add_executable(draco_tests
    reactor_test.cpp
    http_test.cpp
)
target_link_libraries(draco_tests
    PRIVATE
    draco::draco
    GTest::gtest_main
)
include(GoogleTest)
gtest_discover_tests(draco_tests)

# ── qbuem-json integration tests ─────────────────────────────────────────────
# Fetches qbuem-json and runs integration tests that verify:
#   (a) qbuem-json parses correctly against qbuem-stack Request/Response types
#   (b) round-trip (parse → transform → serialize) works end-to-end
FetchContent_Declare(qbuem_json
    GIT_REPOSITORY https://github.com/qbuem/qbuem-json.git
    GIT_TAG        ${QBUEM_JSON_TAG}
)
FetchContent_GetProperties(qbuem_json)
if(NOT qbuem_json_POPULATED)
    message(STATUS "[qbuem-stack] Fetching qbuem-json (tag: ${QBUEM_JSON_TAG})")
    FetchContent_MakeAvailable(qbuem_json)
endif()

if(TARGET qbuem_json::qbuem_json)
    add_executable(qbuem_json_tests
        qbuem_json_test.cpp
    )
    target_link_libraries(qbuem_json_tests
        PRIVATE
        draco::draco
        qbuem_json::qbuem_json
        GTest::gtest_main
    )
    target_compile_definitions(qbuem_json_tests PRIVATE QBUEM_HAS_QBUEM_JSON)
    gtest_discover_tests(qbuem_json_tests)
    message(STATUS "[qbuem-stack] qbuem-json integration tests enabled")
else()
    # Build the test binary anyway (all tests skip at runtime)
    add_executable(qbuem_json_tests
        qbuem_json_test.cpp
    )
    target_link_libraries(qbuem_json_tests
        PRIVATE
        draco::draco
        GTest::gtest_main
    )
    gtest_discover_tests(qbuem_json_tests)
    message(WARNING
        "[qbuem-stack] qbuem-json not found — integration tests will SKIP at runtime.\n"
        "  Set -DQBUEM_JSON_TAG=<tag> or check network access."
    )
endif()
