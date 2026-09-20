add_library(lmdbxx_obj OBJECT lmdbxx.cpp)
target_include_directories(lmdbxx_obj PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lmdbxx_obj PUBLIC lmdb::lmdb lmdbxx::headers tclxx::tclxx Tcl::Tcl)
target_compile_features(lmdbxx_obj PUBLIC cxx_std_17)
set_target_properties(lmdbxx_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(lmdbxx MODULE)
target_link_libraries(lmdbxx PRIVATE lmdbxx_obj)
set_target_properties(lmdbxx PROPERTIES
    PREFIX ""
    OUTPUT_NAME "lmdbxx"
)
# NOTE: pkgIndex.tcl resolves the module file via Tcl's own
# [info sharedlibextension], which must match the MODULE suffix CMake
# picks for this platform (.so on Linux, .dylib on macOS's stock Tcl, .dll
# on Windows). Verified on Linux; cross-check on macOS/Windows before release.

configure_file(
    ${CMAKE_SOURCE_DIR}/pkgIndex.tcl.in
    ${CMAKE_CURRENT_BINARY_DIR}/pkgIndex.tcl
    @ONLY
)

install(TARGETS lmdbxx
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/lmdbxx
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/lmdbxx
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/pkgIndex.tcl
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/lmdbxx
)
