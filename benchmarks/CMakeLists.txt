# benchmarks/CMakeLists.txt
#
# Latency micro-benchmarks. Not part of the normal build - enable with
# -DBUILD_BENCHMARKS=ON. No test framework: each benchmark is a plain
# executable that prints numbers.

add_executable(bench_binance_parser
    bench_binance_parser.cpp
    latency_benchmark.h
)

# The source includes "md_provider/binance/binance_parser.h", so the project
# root must be on the include path (same style as the unit tests).
target_include_directories(bench_binance_parser PRIVATE ${CMAKE_SOURCE_DIR})

target_link_libraries(bench_binance_parser PRIVATE
    md_provider
)

# Order book hot path: per-venue apply, the k-way merge, and the BBO. These
# are the before-numbers the flat-vector book (DESIGN 14.2 step 16) and the
# incremental merge (step 16b) have to be justified against.
#
# Links md_core only - no provider, no networking. The whole point of md_core
# having no I/O is that its cost can be measured without one.
add_executable(bench_md_core
    bench_md_core.cpp
    latency_benchmark.h
)

target_include_directories(bench_md_core PRIVATE ${CMAKE_SOURCE_DIR})

target_link_libraries(bench_md_core PRIVATE
    md_core
)
