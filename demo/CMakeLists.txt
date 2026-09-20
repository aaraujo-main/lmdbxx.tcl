cmake_minimum_required(VERSION 3.16)

if(NOT TCLSH_PATH)
    message(STATUS "lmdbxx demo: tclsh not found, skipping demo smoke tests")
    return()
endif()

set(LMDBXX_DEMO_SCRIPTS
    basic_crud.tcl
    transactions.tcl
    iterators.tcl
    mapsize.tcl
)

foreach(script IN LISTS LMDBXX_DEMO_SCRIPTS)
    get_filename_component(demo_name ${script} NAME_WE)
    add_test(
        NAME demo_${demo_name}
        COMMAND ${TCLSH_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/${script}
    )
    set_tests_properties(demo_${demo_name} PROPERTIES
        ENVIRONMENT "TCLLIBPATH=$<TARGET_FILE_DIR:lmdbxx>"
        LABELS "demo;lmdbxx"
    )
endforeach()
