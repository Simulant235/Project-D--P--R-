cmake_minimum_required(VERSION 3.25)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" DIVA_VERSION_STRING LIMIT_COUNT 1)
string(STRIP "${DIVA_VERSION_STRING}" DIVA_VERSION_STRING)

# keine Vorabversionen in project(): nur x.y.z
string(REGEX MATCH "^[0-9]+\.[0-9]+\.[0-9]+" DIVA_VERSION_CORE "${DIVA_VERSION_STRING}")
project(diva_psp_recomp VERSION ${DIVA_VERSION_CORE} LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/alpha.cmake)
include(cmake/release.cmake)

configure_file(cmake/version.h.in "${CMAKE_BINARY_DIR}/generated/version.h" @ONLY)

foreach(dir recompiler runtime mod installer)   # runtime/hle kommt über runtime
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/CMakeLists.txt")
        add_subdirectory(${dir})
    endif()
endforeach()
