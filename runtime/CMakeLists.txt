# Runtime (T20): Speicher, Threads, Zeit, Lader, Logging + HLE-Lib; Spiel-.exe nur mit generiertem Code.
add_library(diva_runtime STATIC
    src/log.cpp
    src/memory.cpp
    src/kernel.cpp
    src/support.cpp
    src/iso.cpp
    src/loader.cpp
    src/run.cpp
    src/window.cpp
    src/overlay.cpp
    src/pad_ps.cpp
    src/alpha_guard.cpp
    vfpu/psp_vfpu.c
)
target_include_directories(diva_runtime PUBLIC include PRIVATE src)
target_compile_features(diva_runtime PUBLIC cxx_std_20)
target_link_libraries(diva_runtime PUBLIC winmm user32 gdi32 hid setupapi xinput pdh psapi ws2_32 shell32)
set_source_files_properties(src/alpha_guard.cpp PROPERTIES COMPILE_DEFINITIONS
    "DIVA_ALPHA_EXPIRES_X=${DIVA_ALPHA_EXPIRES_X};DIVA_ALPHA_RELEASE_X=${DIVA_ALPHA_RELEASE_X}")
if(MSVC)
    target_compile_options(diva_runtime PRIVATE /W4 /utf-8)
    target_compile_definitions(diva_runtime PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

# SDL3 (optional, py tools/fetch_sdl3.py): alle Controller
set(DIVA_SDL3_DIR "${CMAKE_SOURCE_DIR}/third_party/SDL3")
if(EXISTS "${DIVA_SDL3_DIR}/include/SDL3/SDL.h")
    target_sources(diva_runtime PRIVATE src/pad_sdl.cpp)
    target_include_directories(diva_runtime PRIVATE "${DIVA_SDL3_DIR}/include")
    target_link_libraries(diva_runtime PUBLIC "${DIVA_SDL3_DIR}/lib/x64/SDL3.lib")
    target_compile_definitions(diva_runtime PRIVATE DIVA_HAVE_SDL3)
    message(STATUS "SDL3 gefunden: Controller über SDL3")
else()
    message(STATUS "SDL3 fehlt (py tools/fetch_sdl3.py): Controller nur PS4/PS5 (HID) und Xbox (XInput)")
endif()

add_subdirectory(hle)

if(BUILD_TESTING)
    add_executable(runtime_tests tests/thread_test.cpp)
    target_include_directories(runtime_tests PRIVATE src)
    target_link_libraries(runtime_tests PRIVATE diva_runtime diva_hle)
    if(MSVC)
        target_compile_options(runtime_tests PRIVATE /W4 /utf-8)
    endif()
    add_test(NAME runtime_tests COMMAND runtime_tests)

    add_executable(runtime_sync_tests tests/sync_test.cpp)
    target_include_directories(runtime_sync_tests PRIVATE src)
    target_link_libraries(runtime_sync_tests PRIVATE diva_runtime diva_hle)
    if(MSVC)
        target_compile_options(runtime_sync_tests PRIVATE /W4 /utf-8)
    endif()
    add_test(NAME runtime_sync_tests COMMAND runtime_sync_tests)
endif()

# Spiel-.exe: nur wenn der Recompiler schon Code erzeugt hat (generated/ wird nie eingecheckt).
set(DIVA_GAME "diva1st" CACHE STRING "Spiel, dessen generierter Code gebaut wird")
set(DIVA_GAME_DIR "${CMAKE_SOURCE_DIR}/generated/${DIVA_GAME}" CACHE PATH "Ordner mit dem generierten Code")
if(EXISTS "${DIVA_GAME_DIR}/CMakeLists.txt")
    add_subdirectory("${DIVA_GAME_DIR}" "${CMAKE_BINARY_DIR}/game_${DIVA_GAME}")
    add_executable(${DIVA_GAME} src/exe_main.cpp)
    target_include_directories(${DIVA_GAME} PRIVATE src)
    target_link_libraries(${DIVA_GAME} PRIVATE diva_runtime diva_hle ${DIVA_GAME}_code)
    if(EXISTS "${DIVA_SDL3_DIR}/lib/x64/SDL3.dll")
        add_custom_command(TARGET ${DIVA_GAME} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
                           "${DIVA_SDL3_DIR}/lib/x64/SDL3.dll" "$<TARGET_FILE_DIR:${DIVA_GAME}>")
    endif()
    message(STATUS "Spiel-Exe '${DIVA_GAME}' wird aus ${DIVA_GAME_DIR} gebaut")
else()
    message(STATUS "Kein generierter Code in ${DIVA_GAME_DIR}: Spiel-Exe wird nicht gebaut")
endif()
