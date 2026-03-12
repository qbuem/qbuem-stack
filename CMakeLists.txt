cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Project Configuration
# ============================================================================
project(qbuem-stack
    VERSION 0.3.0
    LANGUAGES CXX
    DESCRIPTION "High-performance Zenith WAS framework (qbuem-stack)"
)


set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================================
# Platform Check (Linux/macOS Only)
# ============================================================================
if(WIN32 OR MSVC)
    message(FATAL_ERROR "Draco WAS does not support Windows.")
endif()

# ============================================================================
# Build Options
# ============================================================================
option(DRACO_BUILD_TESTS    "Build Draco unit tests"   ON)
option(DRACO_BUILD_EXAMPLES "Build Draco examples"     ON)
option(DRACO_INSTALL        "Generate install targets"  ON)

# ============================================================================
# Dependencies (FetchContent — build-time only)
# ============================================================================
include(FetchContent)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ============================================================================
# Helper: shared compile options (PRIVATE — never leak into consumers)
# ============================================================================
function(draco_compile_options target)
    if(NOT MSVC)
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Werror
            -O3 -flto
            $<$<CONFIG:Release>:-march=native>
        )
    endif()
endfunction()

# ============================================================================
# Target: draco::core
#   Reactor + Dispatcher + platform I/O backend (io_uring / kqueue / epoll)
#   No HTTP, no JSON dependency.
# ============================================================================
set(DRACO_CORE_SOURCES
    src/core/reactor.cpp
    src/core/dispatcher.cpp
)

if(APPLE)
    list(APPEND DRACO_CORE_SOURCES src/core/kqueue_reactor.cpp)
else()
    find_path(LIBURING_INCLUDE_DIR liburing.h)
    find_library(LIBURING_LIB NAMES uring)

    if(LIBURING_INCLUDE_DIR AND LIBURING_LIB)
        message(STATUS "io_uring found — using IOUringReactor")
        set(DRACO_USE_IOURING TRUE)
        list(APPEND DRACO_CORE_SOURCES src/core/io_uring_reactor.cpp)
    else()
        message(STATUS "io_uring not found — falling back to EpollReactor")
        list(APPEND DRACO_CORE_SOURCES src/core/epoll_reactor.cpp)
    endif()
endif()

add_library(draco_core STATIC ${DRACO_CORE_SOURCES})
add_library(draco::core ALIAS draco_core)

target_include_directories(draco_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(draco_core PUBLIC cxx_std_23)
draco_compile_options(draco_core)

if(DRACO_USE_IOURING)
    target_compile_definitions(draco_core PUBLIC DRACO_HAS_IOURING)
    target_link_libraries(draco_core PUBLIC ${LIBURING_LIB})
endif()

# ============================================================================
# Target: draco::http
#   HTTP parser, Request, Response, Router.
#   No JSON library dependency — body() is raw bytes; callers choose their
#   own JSON library.
# ============================================================================
add_library(draco_http STATIC
    src/http/parser.cpp
    src/http/request.cpp
    src/http/response.cpp
    src/http/router.cpp
)
add_library(draco::http ALIAS draco_http)

target_link_libraries(draco_http PUBLIC draco_core)
draco_compile_options(draco_http)

# ============================================================================
# Target: draco::draco  (full — App glue)
#   Links draco::core + draco::http.  This is what most users want.
# ============================================================================
add_library(draco STATIC src/draco.cpp)
add_library(draco::draco ALIAS draco)

target_link_libraries(draco PUBLIC draco_core draco_http)
draco_compile_options(draco)

# ============================================================================
# Installation & CMake Package Export
# ============================================================================
if(DRACO_INSTALL)
    install(TARGETS draco_core draco_http draco
        EXPORT  dracoTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    install(DIRECTORY include/draco
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    install(EXPORT dracoTargets
        FILE      dracoTargets.cmake
        NAMESPACE draco::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/draco
    )

    # Substitute @DRACO_BEAST_JSON_TAG@ and @PACKAGE_INIT@ into the config.
    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/draco-config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/draco-config.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/draco
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/draco-config-version.cmake
        VERSION     ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/draco-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/draco-config-version.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/draco
    )
endif()

# ============================================================================
# Subdirectories (Tests / Examples)
# ============================================================================
if(DRACO_BUILD_TESTS)
    enable_testing()
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    add_subdirectory(tests)
endif()

if(DRACO_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
