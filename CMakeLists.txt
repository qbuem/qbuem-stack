cmake_minimum_required(VERSION 3.20)

# ============================================================================
# qbuem-stack — Zero Latency · Zero Cost · Zero Dependency WAS Layer
# ============================================================================
project(qbuem-stack
    VERSION 1.0.0
    LANGUAGES CXX
    DESCRIPTION "qbuem-stack: zero-dependency C++23 async server framework (HTTP/1.1, HTTP/2, WebSocket, gRPC, Pipeline)"
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(QBUEM_JSON_TAG "main" CACHE STRING "qbuem-json git tag / SHA (examples only)")

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
option(QBUEM_BUILD_BENCH    "Build benchmarks"      ON)
option(QBUEM_INSTALL        "Generate install rules" ON)

# ── AF_XDP (optional, requires Linux 4.18+) ─────────────────────────────────
# Usage:
#   cmake -DQBUEM_XDP=ON [-DQBUEM_XDP_LIBBPF=ON] ..
#
# QBUEM_XDP=ON        : 헤더/타겟 노출, QBUEM_HAS_XDP 정의
# QBUEM_XDP_LIBBPF=ON : libbpf 연동 (xsk_umem__create 등 실제 구현 활성화)
option(QBUEM_XDP        "Enable AF_XDP support (Linux 4.18+)"           OFF)
option(QBUEM_XDP_LIBBPF "Link libbpf for AF_XDP (requires QBUEM_XDP=ON)" OFF)

# ── PGO (Profile-Guided Optimization) ───────────────────────────────────────
# Usage:
#   Pass 1 (instrumented):  cmake -DQBUEM_PGO_GENERATE=ON ..  && make && ./bench/bench_throughput ...
#   Pass 2 (optimized):     cmake -DQBUEM_PGO_USE=ON      ..  && make
option(QBUEM_PGO_GENERATE "Build instrumented binary for PGO profiling" OFF)
option(QBUEM_PGO_USE      "Build optimized binary using PGO profile"    OFF)

if(QBUEM_PGO_GENERATE AND QBUEM_PGO_USE)
  message(FATAL_ERROR "QBUEM_PGO_GENERATE and QBUEM_PGO_USE are mutually exclusive")
endif()

if(QBUEM_PGO_GENERATE)
  message(STATUS "[qbuem-stack] PGO: instrumented build (pass 1)")
  add_compile_options(-fprofile-generate=${CMAKE_BINARY_DIR}/pgo-data)
  add_link_options(-fprofile-generate=${CMAKE_BINARY_DIR}/pgo-data)
endif()

if(QBUEM_PGO_USE)
  if(NOT EXISTS "${CMAKE_BINARY_DIR}/pgo-data")
    message(WARNING "[qbuem-stack] PGO: pgo-data directory not found — run pass 1 first")
  else()
    message(STATUS "[qbuem-stack] PGO: optimized build (pass 2, data=${CMAKE_BINARY_DIR}/pgo-data)")
    add_compile_options(-fprofile-use=${CMAKE_BINARY_DIR}/pgo-data -fprofile-correction)
    add_link_options(-fprofile-use=${CMAKE_BINARY_DIR}/pgo-data)
  endif()
endif()

# ── Sanitizers ───────────────────────────────────────────────────────────────
# cmake -DQBUEM_SANITIZE=asan   (ASan + UBSan + LSan)
# cmake -DQBUEM_SANITIZE=tsan   (TSan)
# cmake -DQBUEM_SANITIZE=msan   (MSan, clang only)
# ASan 과 TSan 은 동시 사용 불가.
set(QBUEM_SANITIZE "" CACHE STRING "Sanitizer preset: asan | tsan | msan")

if(QBUEM_SANITIZE STREQUAL "asan")
    message(STATUS "[qbuem-stack] Sanitizer: ASan + UBSan + LeakSanitizer")
    add_compile_options(
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -g -O1
        -fno-optimize-sibling-calls
        -DQBUEM_SANITIZE_ASAN=1
    )
    add_link_options(-fsanitize=address,undefined)
elseif(QBUEM_SANITIZE STREQUAL "tsan")
    message(STATUS "[qbuem-stack] Sanitizer: TSan")
    add_compile_options(
        -fsanitize=thread
        -fno-omit-frame-pointer
        -g -O1
        -DQBUEM_SANITIZE_TSAN=1
    )
    add_link_options(-fsanitize=thread)
elseif(QBUEM_SANITIZE STREQUAL "msan")
    message(STATUS "[qbuem-stack] Sanitizer: MSan (clang only)")
    add_compile_options(
        -fsanitize=memory
        -fno-omit-frame-pointer
        -g -O1
        -DQBUEM_SANITIZE_MSAN=1
    )
    add_link_options(-fsanitize=memory)
endif()

# ── Coverage (gcov/lcov) ──────────────────────────────────────────────────────
# cmake -DQBUEM_COVERAGE=ON
option(QBUEM_COVERAGE "Enable gcov coverage instrumentation" OFF)

if(QBUEM_COVERAGE)
    message(STATUS "[qbuem-stack] Coverage: gcov/lcov enabled")
    add_compile_options(-g -O0 --coverage -fprofile-arcs -ftest-coverage)
    add_link_options(--coverage)
endif()

# ============================================================================
include(FetchContent)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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

# Public include dir (used by all targets)
set(QBUEM_INCLUDE_DIR $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
set(QBUEM_INSTALL_INCLUDE $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# ============================================================================
# ── Level 0: Foundation (header-only, zero dependencies) ────────────────────
# ============================================================================

# qbuem::result — Result<T>, unexpected<E>, errc
add_library(qbuem_result INTERFACE)
add_library(qbuem-stack::result ALIAS qbuem_result)
target_include_directories(qbuem_result INTERFACE ${QBUEM_INCLUDE_DIR} ${QBUEM_INSTALL_INCLUDE})
target_compile_features(qbuem_result INTERFACE cxx_std_23)

# qbuem::arena — Arena, FixedPoolResource, MmapArena, HugePages
add_library(qbuem_arena INTERFACE)
add_library(qbuem-stack::arena ALIAS qbuem_arena)
target_include_directories(qbuem_arena INTERFACE ${QBUEM_INCLUDE_DIR} ${QBUEM_INSTALL_INCLUDE})
target_compile_features(qbuem_arena INTERFACE cxx_std_23)
target_link_libraries(qbuem_arena INTERFACE qbuem_result)

# qbuem::crypto — CSPRNG, constant_time_equal, csrf_token
add_library(qbuem_crypto INTERFACE)
add_library(qbuem-stack::crypto ALIAS qbuem_crypto)
target_include_directories(qbuem_crypto INTERFACE ${QBUEM_INCLUDE_DIR} ${QBUEM_INSTALL_INCLUDE})
target_compile_features(qbuem_crypto INTERFACE cxx_std_23)
target_link_libraries(qbuem_crypto INTERFACE qbuem_result)

# ============================================================================
# ── Level 1: Async Core ─────────────────────────────────────────────────────
# ============================================================================

# qbuem::task — Task<T>, promise_type, symmetric transfer (header-only)
add_library(qbuem_task INTERFACE)
add_library(qbuem-stack::task ALIAS qbuem_task)
target_include_directories(qbuem_task INTERFACE ${QBUEM_INCLUDE_DIR} ${QBUEM_INSTALL_INCLUDE})
target_compile_features(qbuem_task INTERFACE cxx_std_23)
target_link_libraries(qbuem_task INTERFACE qbuem_result)

# qbuem::reactor — Reactor interface + TimerWheel + Dispatcher (static lib)
set(CORE_SOURCES
    src/core/reactor.cpp
    src/core/dispatcher.cpp
)

if(APPLE)
    list(APPEND CORE_SOURCES src/core/kqueue_reactor.cpp)
    message(STATUS "[qbuem-stack] Platform: macOS → KqueueReactor")
else()
    find_path(LIBURING_INCLUDE_DIR liburing.h)
    find_library(LIBURING_LIB NAMES uring)
    if(LIBURING_INCLUDE_DIR AND LIBURING_LIB)
        message(STATUS "[qbuem-stack] io_uring found → IOUringReactor")
        set(QBUEM_HAS_IOURING TRUE)
        list(APPEND CORE_SOURCES src/core/io_uring_reactor.cpp)
    else()
        message(STATUS "[qbuem-stack] io_uring not found → EpollReactor fallback")
        list(APPEND CORE_SOURCES src/core/epoll_reactor.cpp)
    endif()
endif()

add_library(qbuem_reactor STATIC ${CORE_SOURCES})
add_library(qbuem-stack::reactor ALIAS qbuem_reactor)
target_include_directories(qbuem_reactor
    PUBLIC ${QBUEM_INCLUDE_DIR} ${QBUEM_INSTALL_INCLUDE})
target_compile_features(qbuem_reactor PUBLIC cxx_std_23)
target_link_libraries(qbuem_reactor PUBLIC qbuem_task qbuem_arena qbuem_crypto)
qbuem_compile_options(qbuem_reactor)

if(QBUEM_HAS_IOURING)
    target_compile_definitions(qbuem_reactor PUBLIC QBUEM_HAS_IOURING)
    target_link_libraries(qbuem_reactor PUBLIC ${LIBURING_LIB})
endif()

# ============================================================================
# ── Level 2: Reactor Implementations (platform aliases) ─────────────────────
# Platform-specific reactor impls are compiled into qbuem_reactor above.
# Provide named aliases for explicit component linking.
# ============================================================================
add_library(qbuem-stack::epoll   ALIAS qbuem_reactor)
add_library(qbuem-stack::kqueue  ALIAS qbuem_reactor)
add_library(qbuem-stack::iouring ALIAS qbuem_reactor)

# ============================================================================
# ── Level 3: IO Primitives (all header-only) ─────────────────────────────────
# ============================================================================

# qbuem::buf — IOSlice, MutableIOSlice, IOVec<N>, ReadBuf<N>, WriteBuf
add_library(qbuem_buf INTERFACE)
add_library(qbuem-stack::buf ALIAS qbuem_buf)
target_include_directories(qbuem_buf INTERFACE ${QBUEM_INCLUDE_DIR} ${QBUEM_INSTALL_INCLUDE})
target_compile_features(qbuem_buf INTERFACE cxx_std_23)
target_link_libraries(qbuem_buf INTERFACE qbuem_result)

# qbuem::net — SocketAddr, TcpListener, TcpStream, UdpSocket, UnixSocket
add_library(qbuem_net INTERFACE)
add_library(qbuem-stack::net ALIAS qbuem_net)
target_include_directories(qbuem_net INTERFACE ${QBUEM_INCLUDE_DIR} ${QBUEM_INSTALL_INCLUDE})
target_compile_features(qbuem_net INTERFACE cxx_std_23)
target_link_libraries(qbuem_net INTERFACE qbuem_reactor qbuem_buf)

# qbuem::io — AsyncFile, DirectFile, ZeroCopy, SocketOpts, kTLS, io_uring ops
add_library(qbuem_io INTERFACE)
add_library(qbuem-stack::io ALIAS qbuem_io)
target_include_directories(qbuem_io INTERFACE ${QBUEM_INCLUDE_DIR} ${QBUEM_INSTALL_INCLUDE})
target_compile_features(qbuem_io INTERFACE cxx_std_23)
target_link_libraries(qbuem_io INTERFACE qbuem_reactor qbuem_buf qbuem_net)

# qbuem::pcie — PCIeDevice (VFIO), MSIXReactor, DmaBuffer
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(qbuem_pcie STATIC src/pcie/pcie_device.cpp)
    add_library(qbuem-stack::pcie ALIAS qbuem_pcie)
    target_include_directories(qbuem_pcie PUBLIC ${QBUEM_INCLUDE_DIR} ${QBUEM_INSTALL_INCLUDE})
    target_compile_features(qbuem_pcie PUBLIC cxx_std_23)
    target_link_libraries(qbuem_pcie PUBLIC qbuem_result qbuem_reactor)
    qbuem_compile_options(qbuem_pcie)
else()
    add_library(qbuem_pcie INTERFACE)
    add_library(qbuem-stack::pcie ALIAS qbuem_pcie)
    target_include_directories(qbuem_pcie INTERFACE ${QBUEM_INCLUDE_DIR} ${QBUEM_INSTALL_INCLUDE})
endif()

# ============================================================================
# ── Level 4: Transport + Codec + Server (header-only) ───────────────────────
# ============================================================================

# qbuem::transport — ITransport (abstract), PlainTransport
add_library(qbuem_transport INTERFACE)
add_library(qbuem-stack::transport ALIAS qbuem_transport)
target_include_directories(qbuem_transport INTERFACE ${QBUEM_INCLUDE_DIR} ${QBUEM_INSTALL_INCLUDE})
target_compile_features(qbuem_transport INTERFACE cxx_std_23)
target_link_libraries(qbuem_transport INTERFACE qbuem_net qbuem_task qbuem_result)

# qbuem::codec — IFrameCodec, LengthPrefixCodec, LineCodec, Http1Codec
add_library(qbuem_codec INTERFACE)
add_library(qbuem-stack::codec ALIAS qbuem_codec)
target_include_directories(qbuem_codec INTERFACE ${QBUEM_INCLUDE_DIR} ${QBUEM_INSTALL_INCLUDE})
target_compile_features(qbuem_codec INTERFACE cxx_std_23)
target_link_libraries(qbuem_codec INTERFACE qbuem_buf qbuem_result)

# qbuem::server-base — ConnectionHandler base, connection lifecycle
add_library(qbuem_server_base INTERFACE)
add_library(qbuem-stack::server-base ALIAS qbuem_server_base)
target_include_directories(qbuem_server_base INTERFACE ${QBUEM_INCLUDE_DIR} ${QBUEM_INSTALL_INCLUDE})
target_compile_features(qbuem_server_base INTERFACE cxx_std_23)
target_link_libraries(qbuem_server_base INTERFACE
    qbuem_net qbuem_transport qbuem_codec qbuem_reactor qbuem_arena)

# ============================================================================
# ── Level 5: HTTP ────────────────────────────────────────────────────────────
# ============================================================================

# qbuem::http — SIMD HTTP/1.1 FSM parser, Radix Tree router, Request/Response
add_library(qbuem_http STATIC
    src/http/parser.cpp
    src/http/request.cpp
    src/http/response.cpp
    src/http/router.cpp
)
add_library(qbuem-stack::http ALIAS qbuem_http)
target_link_libraries(qbuem_http PUBLIC qbuem_reactor qbuem_buf qbuem_codec)
qbuem_compile_options(qbuem_http)

# qbuem::http-server — Http1Handler + App + middleware chain
add_library(qbuem_http_server INTERFACE)
add_library(qbuem-stack::http-server ALIAS qbuem_http_server)
target_include_directories(qbuem_http_server INTERFACE ${QBUEM_INCLUDE_DIR} ${QBUEM_INSTALL_INCLUDE})
target_compile_features(qbuem_http_server INTERFACE cxx_std_23)
target_link_libraries(qbuem_http_server INTERFACE
    qbuem_http qbuem_server_base qbuem_transport qbuem_arena)

# ============================================================================
# ── Level 6: Pipeline (all header-only) ─────────────────────────────────────
# ============================================================================

# qbuem::context — Context (persistent K/V list), ServiceRegistry, ActionEnv, Concepts
add_library(qbuem_context INTERFACE)
add_library(qbuem-stack::context ALIAS qbuem_context)
target_include_directories(qbuem_context INTERFACE ${QBUEM_INCLUDE_DIR} ${QBUEM_INSTALL_INCLUDE})
target_compile_features(qbuem_context INTERFACE cxx_std_23)
target_link_libraries(qbuem_context INTERFACE qbuem_task qbuem_result)

# qbuem::channel — AsyncChannel<T> (MPMC), SpscChannel, PriorityChannel, Stream<T>, TaskGroup
add_library(qbuem_channel INTERFACE)
add_library(qbuem-stack::channel ALIAS qbuem_channel)
target_include_directories(qbuem_channel INTERFACE ${QBUEM_INCLUDE_DIR} ${QBUEM_INSTALL_INCLUDE})
target_compile_features(qbuem_channel INTERFACE cxx_std_23)
target_link_libraries(qbuem_channel INTERFACE qbuem_task qbuem_reactor qbuem_context)

# qbuem::pipeline — Action<In,Out>, StaticPipeline, DynamicPipeline, PipelineBuilder
add_library(qbuem_pipeline INTERFACE)
add_library(qbuem-stack::pipeline ALIAS qbuem_pipeline)
target_include_directories(qbuem_pipeline INTERFACE ${QBUEM_INCLUDE_DIR} ${QBUEM_INSTALL_INCLUDE})
target_compile_features(qbuem_pipeline INTERFACE cxx_std_23)
target_link_libraries(qbuem_pipeline INTERFACE qbuem_channel qbuem_context qbuem_reactor)

# ============================================================================
# ── Level 7: Pipeline Extensions (all header-only) ──────────────────────────
# ============================================================================

# qbuem::pipeline-graph — PipelineGraph (DAG), MessageBus, HotSwap, ArenaChannel
add_library(qbuem_pipeline_graph INTERFACE)
add_library(qbuem-stack::pipeline-graph ALIAS qbuem_pipeline_graph)
target_include_directories(qbuem_pipeline_graph INTERFACE ${QBUEM_INCLUDE_DIR} ${QBUEM_INSTALL_INCLUDE})
target_compile_features(qbuem_pipeline_graph INTERFACE cxx_std_23)
target_link_libraries(qbuem_pipeline_graph INTERFACE qbuem_pipeline)

# qbuem::resilience — RetryPolicy, CircuitBreaker, DeadLetterQueue,
#                     Saga, IdempotencyFilter, CheckpointStore, Canary, SLO,
#                     WindowedAction, BatchAction, EventActions
add_library(qbuem_resilience INTERFACE)
add_library(qbuem-stack::resilience ALIAS qbuem_resilience)
target_include_directories(qbuem_resilience INTERFACE ${QBUEM_INCLUDE_DIR} ${QBUEM_INSTALL_INCLUDE})
target_compile_features(qbuem_resilience INTERFACE cxx_std_23)
target_link_libraries(qbuem_resilience INTERFACE qbuem_pipeline qbuem_context)

# qbuem::tracing — W3C TraceContext, Span, Sampler, SpanExporter, PipelineTracer
add_library(qbuem_tracing INTERFACE)
add_library(qbuem-stack::tracing ALIAS qbuem_tracing)
target_include_directories(qbuem_tracing INTERFACE ${QBUEM_INCLUDE_DIR} ${QBUEM_INSTALL_INCLUDE})
target_compile_features(qbuem_tracing INTERFACE cxx_std_23)
target_link_libraries(qbuem_tracing INTERFACE qbuem_context)

# qbuem::metrics — ActionMetrics, PipelineMetrics, PipelineObserver
add_library(qbuem_metrics INTERFACE)
add_library(qbuem-stack::metrics ALIAS qbuem_metrics)
target_include_directories(qbuem_metrics INTERFACE ${QBUEM_INCLUDE_DIR} ${QBUEM_INSTALL_INCLUDE})
target_compile_features(qbuem_metrics INTERFACE cxx_std_23)
target_link_libraries(qbuem_metrics INTERFACE qbuem_pipeline qbuem_context)

# ============================================================================
# ── Level 8: Protocol Handlers (header-only — implementation is in headers) ─
# ============================================================================

# qbuem::ws — WebSocketHandler (RFC 6455)
add_library(qbuem_ws INTERFACE)
add_library(qbuem-stack::ws ALIAS qbuem_ws)
target_include_directories(qbuem_ws INTERFACE ${QBUEM_INCLUDE_DIR} ${QBUEM_INSTALL_INCLUDE})
target_compile_features(qbuem_ws INTERFACE cxx_std_23)
target_link_libraries(qbuem_ws INTERFACE qbuem_http_server qbuem_server_base qbuem_buf)

# qbuem::http2 — Http2Handler (HPACK encoder/decoder, stream multiplexing)
add_library(qbuem_http2 INTERFACE)
add_library(qbuem-stack::http2 ALIAS qbuem_http2)
target_include_directories(qbuem_http2 INTERFACE ${QBUEM_INCLUDE_DIR} ${QBUEM_INSTALL_INCLUDE})
target_compile_features(qbuem_http2 INTERFACE cxx_std_23)
target_link_libraries(qbuem_http2 INTERFACE qbuem_http_server qbuem_server_base qbuem_buf qbuem_arena)

# qbuem::grpc — GrpcHandler<Req,Res> (Unary/Server/Client/Bidi streaming)
add_library(qbuem_grpc INTERFACE)
add_library(qbuem-stack::grpc ALIAS qbuem_grpc)
target_include_directories(qbuem_grpc INTERFACE ${QBUEM_INCLUDE_DIR} ${QBUEM_INSTALL_INCLUDE})
target_compile_features(qbuem_grpc INTERFACE cxx_std_23)
target_link_libraries(qbuem_grpc INTERFACE qbuem_http2 qbuem_pipeline qbuem_channel)

# ============================================================================
# ── Level 8b: AF_XDP (optional, Linux 4.18+) ─────────────────────────────────
# cmake -DQBUEM_XDP=ON [-DQBUEM_XDP_LIBBPF=ON] ..
# ============================================================================

add_library(qbuem_xdp INTERFACE)
add_library(qbuem-stack::xdp ALIAS qbuem_xdp)
target_include_directories(qbuem_xdp INTERFACE ${QBUEM_INCLUDE_DIR} ${QBUEM_INSTALL_INCLUDE})
target_compile_features(qbuem_xdp INTERFACE cxx_std_23)
target_link_libraries(qbuem_xdp INTERFACE qbuem_result qbuem_arena)

if(QBUEM_XDP)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(WARNING "[qbuem-stack] QBUEM_XDP=ON is only supported on Linux. Ignoring.")
    else()
        message(STATUS "[qbuem-stack] AF_XDP enabled (QBUEM_HAS_XDP)")
        target_compile_definitions(qbuem_xdp INTERFACE QBUEM_HAS_XDP)

        if(QBUEM_XDP_LIBBPF)
            find_path(LIBBPF_INCLUDE_DIR  NAMES xdp/xsk.h bpf/libbpf.h
                      HINTS /usr/include /usr/local/include)
            find_library(LIBBPF_LIB NAMES bpf
                         HINTS /usr/lib /usr/local/lib)
            find_library(LIBXDP_LIB NAMES xdp
                         HINTS /usr/lib /usr/local/lib)

            if(LIBBPF_INCLUDE_DIR AND LIBBPF_LIB)
                message(STATUS "[qbuem-stack] libbpf found → QBUEM_XDP_LIBBPF enabled")
                target_compile_definitions(qbuem_xdp INTERFACE QBUEM_XDP_LIBBPF)
                target_include_directories(qbuem_xdp INTERFACE ${LIBBPF_INCLUDE_DIR})
                target_link_libraries(qbuem_xdp INTERFACE ${LIBBPF_LIB})
                if(LIBXDP_LIB)
                    message(STATUS "[qbuem-stack] libxdp found → linking")
                    target_link_libraries(qbuem_xdp INTERFACE ${LIBXDP_LIB})
                endif()
            else()
                message(WARNING
                    "[qbuem-stack] QBUEM_XDP_LIBBPF=ON but libbpf not found. "
                    "Install libbpf-dev (Debian/Ubuntu) or libbpf-devel (Fedora). "
                    "Falling back to stub (no-op) implementation.")
            endif()
        endif()
    endif()
endif()

# ============================================================================
# ── Level 9: Umbrella — qbuem-stack::qbuem (everything) ─────────────────────
# ============================================================================

# Full-stack App + StackController (qbuem_stack.cpp)
add_library(qbuem STATIC src/qbuem_stack.cpp)
add_library(qbuem-stack::qbuem ALIAS qbuem)
target_link_libraries(qbuem PUBLIC
    qbuem_reactor
    qbuem_http
    qbuem_http_server
    qbuem_pipeline
    qbuem_pipeline_graph
    qbuem_resilience
    qbuem_tracing
    qbuem_metrics
    qbuem_ws
    qbuem_http2
    qbuem_grpc
    qbuem_xdp
    qbuem_io
    qbuem_net
    qbuem_buf
    qbuem_codec
    qbuem_transport
    qbuem_arena
    qbuem_crypto
    qbuem_result
)
# SHMChannel uses shm_open/shm_unlink — link librt on Linux (glibc < 2.17 requires it)
# qbuem_pcie provides VFIO-based PCIe user-space control on Linux.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(qbuem PUBLIC rt qbuem_pcie)
endif()
qbuem_compile_options(qbuem)

# ============================================================================
# ── Backward-compatible alias targets ────────────────────────────────────────
# 기존 CMake 코드를 변경하지 않아도 동작하도록 alias 유지.
#
#   qbuem-stack::core    →  qbuem-stack::reactor  (IO core layer)
#   qbuem-stack::stack   →  qbuem-stack::qbuem    (전체 스택)
#   qbuem-stack::net     (이미 qbuem_net ALIAS로 등록됨)
# ============================================================================
add_library(qbuem_core_compat INTERFACE)
target_link_libraries(qbuem_core_compat INTERFACE qbuem_reactor)
add_library(qbuem-stack::core ALIAS qbuem_core_compat)

add_library(qbuem_stack_compat INTERFACE)
target_link_libraries(qbuem_stack_compat INTERFACE qbuem)
add_library(qbuem-stack::stack ALIAS qbuem_stack_compat)

# ============================================================================
# Install & CMake Package Export
# ============================================================================
if(QBUEM_INSTALL)
    # All installable targets
    set(QBUEM_INSTALL_TARGETS
        qbuem_result
        qbuem_arena
        qbuem_crypto
        qbuem_task
        qbuem_reactor
        qbuem_buf
        qbuem_net
        qbuem_io
        qbuem_transport
        qbuem_codec
        qbuem_server_base
        qbuem_http
        qbuem_http_server
        qbuem_context
        qbuem_channel
        qbuem_pipeline
        qbuem_pipeline_graph
        qbuem_resilience
        qbuem_tracing
        qbuem_metrics
        qbuem_ws
        qbuem_http2
        qbuem_grpc
        qbuem_xdp
        qbuem
        qbuem_core_compat
        qbuem_stack_compat
    )

    install(TARGETS ${QBUEM_INSTALL_TARGETS}
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
# Tests, Benchmarks & Examples
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

if(QBUEM_BUILD_BENCH)
    add_subdirectory(bench)
endif()

if(QBUEM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
