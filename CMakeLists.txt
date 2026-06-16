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
# ASan and TSan cannot be used simultaneously.
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

# Position-Independent Code — required for shared library builds and
# harmless for static builds (enables downstream shared-lib linking).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ============================================================================
include(FetchContent)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ============================================================================
# Optional Link-Time Optimization (opt-in, OFF by default)
# ----------------------------------------------------------------------------
# LTO is left OFF by default for build portability and correctness:
#   * clang emits LTO objects as LLVM bitcode; archived into a .a they are
#     unreadable by a GNU ld whose LTO plugin is a different LLVM version
#     ("file format not recognized"), which breaks clang+GNU-ld toolchains.
#   * LTO links are memory-hungry and can OOM CI runners on large test suites.
# When enabled, it is applied via CMake's INTERPROCEDURAL_OPTIMIZATION property
# so the correct -flto flags, archiver (llvm-ar/gcc-ar) and linker plugin are
# selected for the active compiler — the portable way to do LTO.
# ============================================================================
option(QBUEM_ENABLE_LTO "Enable interprocedural optimization (LTO) for library targets" OFF)
set(QBUEM_IPO_SUPPORTED OFF)
if(QBUEM_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT QBUEM_IPO_SUPPORTED OUTPUT _qbuem_ipo_msg)
    if(QBUEM_IPO_SUPPORTED)
        message(STATUS "[qbuem-stack] LTO: enabled (interprocedural optimization)")
    else()
        message(WARNING "[qbuem-stack] LTO requested but unsupported: ${_qbuem_ipo_msg}")
    endif()
endif()

# ============================================================================
# Hardware crypto (opt-in, like LTO above)
# ----------------------------------------------------------------------------
# The crypto module (header-only) has hardware paths for SHA-2 (ARM SHA2 /
# x86 SHA-NI) and AES (ARM AES / x86 AES-NI) that are gated on the compiler's
# __ARM_FEATURE_SHA2 / __SHA__ / etc. feature macros. Those macros are NOT set
# by the default Release flags on every toolchain (notably Apple clang's
# -march=native does not enable them on Apple Silicon), so the scalar fallback
# is used unless this option turns the instruction sets on — measured ~11x
# faster SHA-256/HMAC. OFF by default because it makes the produced binary
# require those CPU features (host-targeting build).
# ============================================================================
option(QBUEM_ENABLE_NATIVE_CRYPTO
    "Compile the crypto module with the host CPU's hardware SHA-2/AES instructions (host-targeting; ~11x SHA-256)"
    OFF)
set(QBUEM_NATIVE_CRYPTO_FLAGS "")
if(QBUEM_ENABLE_NATIVE_CRYPTO AND NOT MSVC)
    include(CheckCXXCompilerFlag)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
        # Candidates ordered to enable +crypto WITHOUT downgrading the native
        # baseline. The first that the compiler accepts wins.
        set(_qbuem_crypto_candidates "-march=native+crypto" "-mcpu=native" "-march=armv8.2-a+crypto")
    else()
        # x86: -march=native enables SHA-NI/AES-NI when the host has them.
        set(_qbuem_crypto_candidates "-march=native")
    endif()
    foreach(_f IN LISTS _qbuem_crypto_candidates)
        string(MAKE_C_IDENTIFIER "QBUEM_CRYPTO_FLAG${_f}" _id)
        check_cxx_compiler_flag("${_f}" ${_id})
        if(${_id})
            set(QBUEM_NATIVE_CRYPTO_FLAGS "${_f}")
            break()
        endif()
    endforeach()
    if(QBUEM_NATIVE_CRYPTO_FLAGS)
        message(STATUS "[qbuem-stack] native crypto: enabled via ${QBUEM_NATIVE_CRYPTO_FLAGS}")
    else()
        message(WARNING "[qbuem-stack] QBUEM_ENABLE_NATIVE_CRYPTO is ON but no supported crypto flag was found for this compiler/arch")
    endif()
endif()

# ============================================================================
# Helper: compile options (PRIVATE — never leak to consumers)
# ----------------------------------------------------------------------------
# Optimization level follows CMAKE_BUILD_TYPE (Release => -O3 -DNDEBUG via the
# toolchain default); do NOT hardcode -O3 here, as that would also optimize
# Debug / sanitizer builds and degrade their diagnostics.
# ============================================================================
function(qbuem_compile_options target)
    if(NOT MSVC)
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Werror
            $<$<CONFIG:Release>:-march=native>
        )
    endif()
    if(QBUEM_ENABLE_LTO AND QBUEM_IPO_SUPPORTED)
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
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
# Hardware crypto instruction sets (opt-in via QBUEM_ENABLE_NATIVE_CRYPTO).
# INTERFACE so the header-only crypto compiles with the feature macros set in
# every consuming TU (it propagates transitively through reactor → umbrella).
if(QBUEM_NATIVE_CRYPTO_FLAGS)
    target_compile_options(qbuem_crypto INTERFACE ${QBUEM_NATIVE_CRYPTO_FLAGS})
endif()

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

add_library(qbuem_reactor ${CORE_SOURCES})
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
add_library(qbuem_http
    src/http/parser.cpp
    src/http/request.cpp
    src/http/response.cpp
    src/http/router.cpp
)
add_library(qbuem-stack::http ALIAS qbuem_http)
# http/ headers also include net/, io/, pipeline/ and tracing/ — declare them so
# `qbuem-stack::http` is independently linkable (matches the actual #includes).
target_link_libraries(qbuem_http PUBLIC
    qbuem_reactor qbuem_buf qbuem_net qbuem_io qbuem_pipeline qbuem_tracing)
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
# tracing/trace_context.hpp uses qbuem::random_bytes (crypto) for span/trace IDs.
target_link_libraries(qbuem_tracing INTERFACE qbuem_context qbuem_crypto)

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
# ── Level 9: Umbrella — qbuem-stack::qbuem (everything) ─────────────────────
# ============================================================================

# Full-stack App + StackController (qbuem_stack.cpp)
add_library(qbuem src/qbuem_stack.cpp)
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
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(qbuem PUBLIC rt)
endif()
qbuem_compile_options(qbuem)

# ============================================================================
# ── Backward-compatible alias targets ────────────────────────────────────────
# Maintain aliases so that existing CMake consumers need no changes.
#
#   qbuem-stack::core    →  qbuem-stack::reactor  (IO core layer)
#   qbuem-stack::stack   →  qbuem-stack::qbuem    (full stack)
#   qbuem-stack::net     (already registered as qbuem_net ALIAS)
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
