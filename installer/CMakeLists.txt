# T41 launcher
add_executable(diva_launcher WIN32 launcher.cpp ${CMAKE_SOURCE_DIR}/runtime/src/alpha_guard.cpp)
target_include_directories(diva_launcher PRIVATE ${CMAKE_SOURCE_DIR}/runtime/include)
set_source_files_properties(${CMAKE_SOURCE_DIR}/runtime/src/alpha_guard.cpp TARGET_DIRECTORY diva_launcher PROPERTIES COMPILE_DEFINITIONS
    "DIVA_ALPHA_EXPIRES_X=${DIVA_ALPHA_EXPIRES_X};DIVA_ALPHA_RELEASE_X=${DIVA_ALPHA_RELEASE_X}")
set_target_properties(diva_launcher PROPERTIES OUTPUT_NAME "DIVA Launcher")
target_compile_definitions(diva_launcher PRIVATE UNICODE _UNICODE DIVA_VERSION_TEXT="${DIVA_VERSION_STRING}"
    DIVA_UPDATE_REPO="${DIVA_UPDATE_REPO}" DIVA_REPORT_TO="${DIVA_REPORT_TO}")
target_link_libraries(diva_launcher PRIVATE comctl32 shell32 ole32 uuid comdlg32 gdi32 user32 gdiplus dwmapi uxtheme advapi32 ws2_32 winhttp bcrypt)

# setup = stub + Paket (make_setup.py)
add_executable(diva_setup WIN32 setup.cpp)
set_target_properties(diva_setup PROPERTIES OUTPUT_NAME "DIVA-Setup-stub")
target_compile_definitions(diva_setup PRIVATE UNICODE _UNICODE DIVA_VERSION_TEXT="${DIVA_VERSION_STRING}")
target_link_libraries(diva_setup PRIVATE comctl32 shell32 ole32 uuid gdi32 user32 advapi32 dwmapi)

set_property(TARGET diva_launcher diva_setup PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
    target_compile_options(diva_launcher PRIVATE /utf-8)
    target_compile_options(diva_setup PRIVATE /utf-8)
endif()
