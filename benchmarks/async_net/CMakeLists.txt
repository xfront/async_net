# TechEmpower Framework Benchmarks — async_net
# Build: add_subdirectory(benchmarks) in root CMakeLists.txt
# Or build standalone: cmake -B build-bench && cmake --build build-bench

cmake_minimum_required(VERSION 3.20)

# If building standalone (not as part of async_net)
if(NOT TARGET async_net)
    project(async_net_benchmarks LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    # Find parent project
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. ${CMAKE_CURRENT_BINARY_DIR}/async_net)
endif()

# Compiler-specific coroutine flags
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-fcoroutines)
endif()

# Benchmark server executable (multi-process version)
add_executable(te_bench benchmark_server.cpp)
target_link_libraries(te_bench PRIVATE async_net)
target_include_directories(te_bench PRIVATE ${CMAKE_SOURCE_DIR}/src)

# Multi-threaded benchmark server executable
add_executable(te_bench_mt benchmark_server_mt.cpp)
target_link_libraries(te_bench_mt PRIVATE async_net)
target_include_directories(te_bench_mt PRIVATE ${CMAKE_SOURCE_DIR}/src)

# Optimization flags for benchmarking
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    target_compile_options(te_bench PRIVATE -O3 -march=native)
    target_compile_options(te_bench_mt PRIVATE -O3 -march=native)
endif()
