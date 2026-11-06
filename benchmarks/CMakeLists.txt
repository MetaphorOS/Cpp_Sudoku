# sudoku_bench: measurement tool of the solver study (see benchmarks/README.md).
add_executable(sudoku_bench
    src/bench_main.cpp
    src/common.cpp
    src/fixtures.cpp
)
target_link_libraries(sudoku_bench PRIVATE sudoku_core)
if(WIN32)
    target_link_libraries(sudoku_bench PRIVATE psapi)
endif()
sudoku_set_warnings(sudoku_bench)

# Recorded in every benchmark's environment metadata.
string(TOUPPER "${CMAKE_BUILD_TYPE}" SUDOKU_BENCH_BUILD_TYPE_UPPER)
target_compile_definitions(sudoku_bench PRIVATE
    SUDOKU_BENCH_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    SUDOKU_BENCH_CXX_FLAGS="${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${SUDOKU_BENCH_BUILD_TYPE_UPPER}}"
)
# Next to the other executables (build-cmake/sudoku_bench).
set_target_properties(sudoku_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
