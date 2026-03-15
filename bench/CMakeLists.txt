# ============================================================================
# qbuem-stack benchmarks
# ============================================================================
# 성능 목표 (v1.0):
#   - HTTP 파서    : > 500 MB/s (1 GiB/s @AVX2)
#   - 라우터 룩업  : < 200 ns/req (1000-route 트리)
#   - Arena 할당   : < 5 ns/alloc (bump-pointer)
#   - AsyncChannel : > 10M ops/s (MPMC ring-buffer)
#   - Pipeline     : > 1M items/s (3-stage chain)
# ============================================================================

add_executable(bench_http
    bench_http_parser.cpp
)
target_link_libraries(bench_http PRIVATE qbuem-stack::qbuem)
target_compile_options(bench_http PRIVATE -O3 -march=native)

add_executable(bench_router
    bench_router.cpp
)
target_link_libraries(bench_router PRIVATE qbuem-stack::qbuem)
target_compile_options(bench_router PRIVATE -O3 -march=native)

add_executable(bench_arena
    bench_arena.cpp
)
target_link_libraries(bench_arena PRIVATE qbuem-stack::qbuem)
target_compile_options(bench_arena PRIVATE -O3 -march=native)

add_executable(bench_channel
    bench_channel.cpp
)
target_link_libraries(bench_channel PRIVATE qbuem-stack::qbuem)
target_compile_options(bench_channel PRIVATE -O3 -march=native)

add_executable(bench_pipeline
    bench_pipeline.cpp
)
target_link_libraries(bench_pipeline PRIVATE qbuem-stack::qbuem)
target_compile_options(bench_pipeline PRIVATE -O3 -march=native)

# Umbrella target: 모든 벤치마크를 순서대로 실행
add_custom_target(run_bench
    COMMAND bench_http
    COMMAND bench_router
    COMMAND bench_arena
    COMMAND bench_channel
    COMMAND bench_pipeline
    DEPENDS bench_http bench_router bench_arena bench_channel bench_pipeline
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bench
    COMMENT "[qbuem-stack] Running all benchmarks..."
)
