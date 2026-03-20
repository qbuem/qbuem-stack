# ============================================================================
# qbuem-stack benchmarks
# ============================================================================
# Performance goals (v3.3.0):
#   - HTTP parser     : > 300 MB/s (SIMD scalar path)
#   - Router lookup   : < 200 ns/req (1000-route radix tree)
#   - Arena alloc     : < 20 ns/alloc (bump-pointer)
#   - AsyncChannel    : > 40M ops/s (MPMC ring-buffer)
#   - SpscChannel     : > 100M ops/s (wait-free SPSC)
#   - GridBitset test : < 3 ns (wait-free atomic load)
#   - GridBitset ray  : < 100 ns (32-step DDA)
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

add_executable(bench_grid
    bench_grid.cpp
)
target_link_libraries(bench_grid PRIVATE qbuem-stack::qbuem)
target_compile_options(bench_grid PRIVATE -O3 -march=native)

# Umbrella target: run all benchmarks in sequence
add_custom_target(run_bench
    COMMAND bench_http
    COMMAND bench_router
    COMMAND bench_arena
    COMMAND bench_channel
    COMMAND bench_pipeline
    COMMAND bench_grid
    DEPENDS bench_http bench_router bench_arena bench_channel bench_pipeline bench_grid
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bench
    COMMENT "[qbuem-stack] Running all benchmarks..."
)
