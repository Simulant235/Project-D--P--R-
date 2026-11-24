set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_library(diva_recomp_core STATIC
    src/config.cpp
    src/module.cpp
    src/mips.cpp
    src/analyze.cpp
    src/emit.cpp
)
target_include_directories(diva_recomp_core PUBLIC src)
target_compile_features(diva_recomp_core PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(diva_recomp_core PRIVATE /W4 /utf-8)
    target_compile_definitions(diva_recomp_core PUBLIC _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(diva_recomp src/main.cpp)
target_link_libraries(diva_recomp PRIVATE diva_recomp_core)
if(MSVC)
    target_compile_options(diva_recomp PRIVATE /W4 /utf-8)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
