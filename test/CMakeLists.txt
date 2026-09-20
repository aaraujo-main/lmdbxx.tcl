cmake_minimum_required(VERSION 3.16)

include(FetchContent)
include(GoogleTest)

find_package(GTest CONFIG QUIET)
if(NOT GTest_FOUND)
    FetchContent_Declare(
        googletest
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(INSTALL_GMOCK OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

add_executable(lmdbxx_test lmdbxx_test.cpp)
target_compile_features(lmdbxx_test PRIVATE cxx_std_17)
target_link_libraries(lmdbxx_test PRIVATE lmdbxx_obj GTest::gtest_main)

gtest_discover_tests(
    lmdbxx_test
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "unit;lmdbxx"
)
