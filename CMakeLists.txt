cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Project Configuration
# ============================================================================
project(draco-was 
    VERSION 0.2.0 
    LANGUAGES CXX
    DESCRIPTION "High-performance, WAS framework for C++23"
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================================
# Platform Check (Linux/macOS Only)
# ============================================================================
if(WIN32 OR MSVC)
    message(FATAL_ERROR "Draco WAS does not support Windows. Only Linux and macOS are supported.")
endif()

# ============================================================================
# Build Options
# ============================================================================
option(DRACO_BUILD_TESTS "Build Draco unit tests" ON)
option(DRACO_BUILD_EXAMPLES "Build Draco examples" ON)
option(DRACO_INSTALL "Generate install targets" ON)

# ============================================================================
# Dependencies (FetchContent)
# ============================================================================
include(FetchContent)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# 1. Beast JSON
set(BEAST_JSON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    beast_json
    GIT_REPOSITORY https://github.com/the-lkb/beast-json.git
    GIT_TAG main
)
FetchContent_MakeAvailable(beast_json)

# ============================================================================
# Library Target (draco)
# ============================================================================
set(DRACO_SOURCES
    src/draco.cpp
    src/core/reactor.cpp
    src/core/dispatcher.cpp
    src/http/parser.cpp
    src/http/request.cpp
    src/http/response.cpp
    src/http/router.cpp
)

if(APPLE)
    list(APPEND DRACO_SOURCES src/core/kqueue_reactor.cpp)
else()
    # Prefer io_uring (Linux ≥ 5.1) for true async I/O; fall back to epoll.
    find_path(LIBURING_INCLUDE_DIR liburing.h)
    find_library(LIBURING_LIB NAMES uring)

    if(LIBURING_INCLUDE_DIR AND LIBURING_LIB)
        message(STATUS "io_uring found: ${LIBURING_LIB} — using IOUringReactor")
        set(DRACO_USE_IOURING TRUE)
        list(APPEND DRACO_SOURCES src/core/io_uring_reactor.cpp)
    else()
        message(STATUS "io_uring not found — falling back to EpollReactor")
        list(APPEND DRACO_SOURCES src/core/epoll_reactor.cpp)
    endif()
endif()

add_library(draco STATIC ${DRACO_SOURCES})
add_library(draco::draco ALIAS draco)

if(DRACO_USE_IOURING)
    target_compile_definitions(draco PUBLIC DRACO_HAS_IOURING)
    target_link_libraries(draco PUBLIC ${LIBURING_LIB})
endif()

target_include_directories(draco
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(draco PUBLIC cxx_std_23)

# beast-json dependency
target_link_libraries(draco PUBLIC beast_json::beast_json)

# Compile Options
if(NOT MSVC)
    target_compile_options(draco PRIVATE
        -Wall -Wextra -Wpedantic -Werror
        -O3 -march=native -flto
    )
endif()

# ============================================================================
# Installation & Export
# ============================================================================
if(DRACO_INSTALL)
    # Installation
    install(TARGETS draco
        EXPORT dracoTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    install(DIRECTORY include/draco DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT dracoTargets
        FILE dracoTargets.cmake
        NAMESPACE draco::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/draco
    )

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/draco-config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/draco-config.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/draco
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/draco-config-version.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/draco-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/draco-config-version.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/draco
    )
endif()

# ============================================================================
# Subdirectories (Tests/Examples)
# ============================================================================
if(DRACO_BUILD_TESTS)
    enable_testing()
    
    # 2. Google Test
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    
    add_subdirectory(tests)
endif()

if(DRACO_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
