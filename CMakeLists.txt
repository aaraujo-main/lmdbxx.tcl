cmake_minimum_required(VERSION 3.16)

project(lmdbxx_tcl VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

option(LMDBXX_TCL_BUILD_TESTS "Build the GoogleTest-based test suite" ON)
option(LMDBXX_TCL_BUILD_DEMO "Register demo/*.tcl scripts as smoke tests" ON)
option(LMDBXX_TCL_BUILD_BENCHMARK "Add Tcl dict/LMDB/SQLite benchmark target" OFF)

include(GNUInstallDirs)

# ---- Tcl -------------------------------------------------------------------

find_package(TCL QUIET)

if(NOT TARGET Tcl::Tcl)
    if(TCL_INCLUDE_PATH AND TCL_LIBRARY)
        add_library(Tcl::Tcl INTERFACE IMPORTED)
        set_target_properties(Tcl::Tcl PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${TCL_INCLUDE_PATH}"
            INTERFACE_LINK_LIBRARIES "${TCL_LIBRARY}"
        )
    else()
        find_path(LMDBXX_TCL_TCL_INCLUDE_DIR tcl.h REQUIRED)
        find_library(LMDBXX_TCL_TCL_LIBRARY NAMES tcl tcl8.7 tcl8.6 REQUIRED)
        add_library(Tcl::Tcl INTERFACE IMPORTED)
        set_target_properties(Tcl::Tcl PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${LMDBXX_TCL_TCL_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES "${LMDBXX_TCL_TCL_LIBRARY}"
        )
    endif()
endif()

find_program(TCLSH_PATH NAMES tclsh tclsh8.6 tclsh8.7)
if(NOT TCLSH_PATH AND TCL_TCLSH)
    set(TCLSH_PATH "${TCL_TCLSH}")
endif()

# ---- vendored LMDB (mdb.c + midl.c + module.c) ------------------------------

set(LMDBXX_TCL_LMDB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lmdb/libraries/liblmdb)

add_library(lmdb_static STATIC
    ${LMDBXX_TCL_LMDB_DIR}/mdb.c
    ${LMDBXX_TCL_LMDB_DIR}/midl.c
    ${LMDBXX_TCL_LMDB_DIR}/module.c
)
set_target_properties(lmdb_static PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME lmdb
)
target_include_directories(lmdb_static PUBLIC ${LMDBXX_TCL_LMDB_DIR})

if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(lmdb_static PUBLIC Threads::Threads)
    if(NOT APPLE)
        target_link_libraries(lmdb_static PUBLIC ${CMAKE_DL_LIBS})
    endif()
endif()

add_library(lmdb::lmdb ALIAS lmdb_static)

# ---- lmdb++ (header-only) ---------------------------------------------------

add_library(lmdbxx_headers INTERFACE)
target_include_directories(lmdbxx_headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lmdbxx/include>
)
target_compile_features(lmdbxx_headers INTERFACE cxx_std_17)
add_library(lmdbxx::headers ALIAS lmdbxx_headers)

# ---- tclxx (header-only) -----------------------------------------------------

add_subdirectory(tclxx/include/tclxx)

# ---- extension sources / tests / demos --------------------------------------

add_subdirectory(src)

if(LMDBXX_TCL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

if(LMDBXX_TCL_BUILD_TESTS OR LMDBXX_TCL_BUILD_DEMO)
    if(NOT LMDBXX_TCL_BUILD_TESTS)
        enable_testing()
    endif()
    add_subdirectory(demo)
endif()

if(LMDBXX_TCL_BUILD_BENCHMARK)
    add_subdirectory(benchmark)
endif()
