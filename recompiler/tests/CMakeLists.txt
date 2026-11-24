# Ausführungstests: synthetisches MIPS -> Recompiler -> C -> bauen -> laufen
add_executable(recomp_test_gen test_gen.cpp)
target_link_libraries(recomp_test_gen PRIVATE diva_recomp_core)
if(MSVC)
    target_compile_options(recomp_test_gen PRIVATE /W4 /utf-8)
endif()

set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")
set(RT_INCLUDE "${CMAKE_SOURCE_DIR}/runtime/include")
add_custom_command(
    OUTPUT "${GEN_DIR}/code/fn_000.c" "${GEN_DIR}/code/module.c" "${GEN_DIR}/test_defs.h"
    COMMAND recomp_test_gen "${GEN_DIR}" "${RT_INCLUDE}"
    DEPENDS recomp_test_gen
    COMMENT "Testprogramm recompilieren"
    VERBATIM
)

add_executable(recomp_exec_tests exec_tests.c "${CMAKE_SOURCE_DIR}/runtime/vfpu/psp_vfpu.c" "${GEN_DIR}/code/fn_000.c" "${GEN_DIR}/code/module.c" "${GEN_DIR}/test_defs.h")
set_target_properties(recomp_exec_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_include_directories(recomp_exec_tests PRIVATE "${RT_INCLUDE}" "${GEN_DIR}" "${GEN_DIR}/code")
if(MSVC)
    target_compile_options(recomp_exec_tests PRIVATE /W3 /wd4102 /wd4702)
endif()

add_test(NAME recomp_exec_tests COMMAND recomp_exec_tests)
