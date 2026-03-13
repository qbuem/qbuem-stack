cmake_minimum_required(VERSION 3.20)

# ============================================================================
# qbuem-stack — Zero Latency · Zero Cost · Zero Dependency WAS Layer
# ============================================================================
project(qbuem-stack
    VERSION 0.4.0
    LANGUAGES CXX
    DESCRIPTION "qbuem-stack: zero-dependency C++20 WAS layer (HTTP core + async IO)"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================================
# Platform Check
# ============================================================================
if(WIN32 OR MSVC)
    message(FATAL_ERROR "qbuem-stack does not support Windows (POSIX only).")
endif()

# ============================================================================
# Build Options
# ============================================================================
option(QBUEM_BUILD_TESTS    "Build unit tests"      ON)
option(QBUEM_BUILD_EXAMPLES "Build examples"        ON)
option(QBUEM_INSTALL        "Generate install rules" ON)


# ============================================================================
include(FetchContent)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(QBUEM_JSON_TAG "main" CACHE STRING "qbuem-json git tag / SHA (tests only)")

# ============================================================================
# Helper: compile options (PRIVATE — never leak to consumers)
# ============================================================================
function(qbuem_compile_options target)
    if(NOT MSVC)
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Werror
            -O3 -flto
            $<$<CONFIG:Release>:-march=native>
        )
    endif()
endfunction()

# ============================================================================
# Layer 0+1: qbuem-stack::core
#   Platform IO: io_uring (Linux) / kqueue (macOS) / epoll fallback
#   Async reactor, thread dispatcher, arena allocator, Task coroutine,
#   SPSC async logger.  Zero external dependency.
# ============================================================================
set(CORE_SOURCES
    src/core/reactor.cpp
    src/core/dispatcher.cpp
)

if(APPLE)
    list(APPEND CORE_SOURCES src/core/kqueue_reactor.cpp)
else()
    find_path(LIBURING_INCLUDE_DIR liburing.h)
    find_library(LIBURING_LIB NAMES uring)
    if(LIBURING_INCLUDE_DIR AND LIBURING_LIB)
        message(STATUS "[qbuem-stack] io_uring found -> IOUringReactor")
        set(QBUEM_HAS_IOURING TRUE)
        list(APPEND CORE_SOURCES src/core/io_uring_reactor.cpp)
    else()
        message(STATUS "[qbuem-stack] io_uring not found -> EpollReactor fallback")
        list(APPEND CORE_SOURCES src/core/epoll_reactor.cpp)
    endif()
endif()

add_library(qbuem_core STATIC ${CORE_SOURCES})
add_library(qbuem-stack::core ALIAS qbuem_core)

target_include_directories(qbuem_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(qbuem_core PUBLIC cxx_std_20)
qbuem_compile_options(qbuem_core)

if(QBUEM_HAS_IOURING)
    target_compile_definitions(qbuem_core PUBLIC QBUEM_HAS_IOURING)
    target_link_libraries(qbuem_core PUBLIC ${LIBURING_LIB})
endif()

# ============================================================================
# Layer 2: qbuem-stack::http
#   SIMD HTTP/1.1 FSM parser, Radix Tree router, middleware pipeline,
#   Request/Response value types.  Zero external dependency.
# ============================================================================
add_library(qbuem_http STATIC
    src/http/parser.cpp
    src/http/request.cpp
    src/http/response.cpp
    src/http/router.cpp
)
add_library(qbuem-stack::http ALIAS qbuem_http)

target_link_libraries(qbuem_http PUBLIC qbuem_core)
qbuem_compile_options(qbuem_http)

# ============================================================================
# Layer 3+4: qbuem-stack (full — App + built-in middleware + lifecycle)
#   All extension points (compression, auth, DB) are abstract interfaces.
#   Zero external dependency.
# ============================================================================
add_library(qbuem STATIC src/qbuem_stack.cpp)
add_library(qbuem-stack::qbuem ALIAS qbuem)

target_link_libraries(qbuem PUBLIC qbuem_core qbuem_http)
qbuem_compile_options(qbuem)

# ============================================================================
# Install & CMake Package Export
# ============================================================================
if(QBUEM_INSTALL)
    install(TARGETS qbuem_core qbuem_http qbuem
        EXPORT  qbuemTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(DIRECTORY include/qbuem
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(EXPORT qbuemTargets
        FILE      qbuem-stack-targets.cmake
        NAMESPACE qbuem-stack::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qbuem-stack
    )
    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/qbuem-stack-config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/qbuem-stack-config.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qbuem-stack
    )
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/qbuem-stack-config-version.cmake
        VERSION     ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/qbuem-stack-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/qbuem-stack-config-version.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qbuem-stack
    )
endif()

# ============================================================================
# Tests & Examples
# ============================================================================
if(QBUEM_BUILD_TESTS)
    enable_testing()
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    add_subdirectory(tests)
endif()

if(QBUEM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
