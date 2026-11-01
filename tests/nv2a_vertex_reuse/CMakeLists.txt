cmake_minimum_required(VERSION 3.20)
project(nv2a_vertex_reuse_regression C)
enable_testing()
add_executable(nv2a-vertex-reuse-test nv2a_vertex_reuse_test.c
    ../../src/kernel/nv2a_pb_exec.c
    ../../src/kernel/nv2a_pb_defer.c
    ../../src/kernel/nv2a_vsh_interp.c
    ../../src/kernel/nv2a_combiner.c
    ../../src/kernel/nv2a_flip_hold.c)
target_include_directories(nv2a-vertex-reuse-test PRIVATE ../../src/kernel ../../src)
if(MSVC)
    target_compile_definitions(nv2a-vertex-reuse-test PRIVATE _CRT_SECURE_NO_WARNINGS)
elseif(NOT WIN32)
    target_link_libraries(nv2a-vertex-reuse-test PRIVATE m pthread)
endif()
add_test(NAME reuse_off COMMAND nv2a-vertex-reuse-test)
set_tests_properties(reuse_off PROPERTIES ENVIRONMENT "RECOMP_VSH_VERTEX_CACHE=0")
add_test(NAME reuse_on COMMAND nv2a-vertex-reuse-test)
set_tests_properties(reuse_on PROPERTIES ENVIRONMENT "RECOMP_VSH_VERTEX_CACHE=1")

# Count real interpreter executions without modifying production code.
set_source_files_properties(../../src/kernel/nv2a_vsh_interp.c PROPERTIES
    COMPILE_DEFINITIONS "nv2a_vsh_run=nv2a_vsh_run_reference")
add_test(NAME reuse_default COMMAND ${CMAKE_COMMAND} -E env --unset=RECOMP_VSH_VERTEX_CACHE $<TARGET_FILE:nv2a-vertex-reuse-test>)
