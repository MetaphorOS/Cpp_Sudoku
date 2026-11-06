cmake_minimum_required(VERSION 3.20)
project(Sudoku VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SUDOKU_BUILD_TESTS "Build the unit tests" ON)
# Testing aid: DLXSolver checks its linked structure's invariants during every run (slow).
option(SUDOKU_DLX_VALIDATE "Check DLX invariants at run time (testing only)" OFF)
# Profiling aid: solvers count fine-grained operations into SolveResult::counters (slower).
option(SUDOKU_SOLVER_COUNTERS "Collect solver operation counters (profiling builds only)" OFF)
option(SUDOKU_BUILD_BENCHMARKS "Build the sudoku_bench benchmark tool" ON)

function(sudoku_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    endif()
endfunction()

# Core library: board, constraints, solvers, logical solver, difficulty grader, generator, optional text tracers. Everything else links this.
add_library(sudoku_core
    src/board.cpp
    src/backtracking_solver.cpp
    src/dlx_solver.cpp
    src/auto_solver.cpp
    src/generator.cpp
    src/logical_solver.cpp
    src/difficulty.cpp
    src/text_trace.cpp
)
target_include_directories(sudoku_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(sudoku_core PUBLIC cxx_std_20)
sudoku_set_warnings(sudoku_core)
if(SUDOKU_DLX_VALIDATE)
    target_compile_definitions(sudoku_core PUBLIC SUDOKU_DLX_VALIDATE)
endif()
if(SUDOKU_SOLVER_COUNTERS)
    target_compile_definitions(sudoku_core PUBLIC SUDOKU_SOLVER_COUNTERS)
endif()

# CLI logic as a library so the tests can drive it in-process; main.cpp only forwards argv.
add_library(sudoku_cli app/cli.cpp)
target_include_directories(sudoku_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/app)
target_link_libraries(sudoku_cli PUBLIC sudoku_core)
sudoku_set_warnings(sudoku_cli)

add_executable(sudoku app/main.cpp)
target_link_libraries(sudoku PRIVATE sudoku_cli)
sudoku_set_warnings(sudoku)

if(SUDOKU_BUILD_TESTS)
    enable_testing()

    add_executable(sudoku_tests tests/test_main.cpp tests/test_difficulty.cpp)
    target_link_libraries(sudoku_tests PRIVATE sudoku_cli)
    sudoku_set_warnings(sudoku_tests)
    add_test(NAME unit_tests COMMAND sudoku_tests)

    # End-to-end checks of the real executable (argv handling, exit codes); the CLI
    # behaviour itself is tested in-process by sudoku_tests.
    add_test(NAME cli_generate COMMAND sudoku generate --size 3 --seed 1)
    add_test(NAME cli_solve COMMAND sudoku solve
        "530070000600195000098000060800060003400803001700020006060000280000419005000080079")
    add_test(NAME cli_rejects_bad_input COMMAND sudoku solve "12x")
    set_tests_properties(cli_rejects_bad_input PROPERTIES WILL_FAIL TRUE)
    # Seed 549 reaches hard on its first attempt.
    add_test(NAME cli_generate_difficulty COMMAND sudoku generate --seed 549 --difficulty hard)
    add_test(NAME cli_rate COMMAND sudoku rate
        "530070000600195000098000060800060003400803001700020006060000280000419005000080079")
    set_tests_properties(unit_tests cli_generate cli_solve cli_rejects_bad_input cli_generate_difficulty cli_rate
        PROPERTIES LABELS "unit")

    # Randomized invariant checks over many seeds: slower, so labelled for selection.
    #   ctest -L unit      fast tests only      ctest -L property   property tests only
    add_executable(sudoku_property_tests tests/property_tests.cpp)
    target_link_libraries(sudoku_property_tests PRIVATE sudoku_core)
    sudoku_set_warnings(sudoku_property_tests)
    add_test(NAME property_tests COMMAND sudoku_property_tests)
    set_tests_properties(property_tests PROPERTIES LABELS "property;slow" TIMEOUT 900)
endif()

if(SUDOKU_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
