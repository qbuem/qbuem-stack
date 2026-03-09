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
