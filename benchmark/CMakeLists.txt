cmake_minimum_required(VERSION 3.16)

if(NOT TCLSH_PATH)
    message(FATAL_ERROR "lmdbxx benchmark requires tclsh")
endif()

set(LMDBXX_BENCHMARK_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/benchmark.tcl)

add_custom_target(lmdbxx_benchmark
    COMMAND ${CMAKE_COMMAND} -E env
        "TCLLIBPATH=$<TARGET_FILE_DIR:lmdbxx>"
        ${TCLSH_PATH} ${LMDBXX_BENCHMARK_SCRIPT}
    DEPENDS lmdbxx
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    USES_TERMINAL
    COMMENT "Running Tcl dict/LMDB/SQLite benchmark"
)

if(BUILD_TESTING)
    add_test(
        NAME benchmark_lmdbxx
        COMMAND ${CMAKE_COMMAND} -E env
            "TCLLIBPATH=$<TARGET_FILE_DIR:lmdbxx>"
            ${TCLSH_PATH} ${LMDBXX_BENCHMARK_SCRIPT} -records 1000 -runs 1
    )
    set_tests_properties(benchmark_lmdbxx PROPERTIES LABELS "benchmark;lmdbxx")
endif()