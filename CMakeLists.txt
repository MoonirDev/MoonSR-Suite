cmake_minimum_required(VERSION 3.24)
# The version lives here only. It flows into src/core/version.h (generated), the .exe version resources, the
# Settings page and the installer. Bump it for every release (see README "Releases").
project(MoonSR VERSION 1.1.0 LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "MoonSR currently builds with MSVC only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# Static C/C++ runtime: the shipped programs do not need the Visual C++ Redistributable (the installer is offline).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
add_compile_options(/W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus)

# All module headers are included as "module/header.h" relative to src/.
set(MOONSR_SRC_DIR ${CMAKE_SOURCE_DIR}/src)
configure_file(${CMAKE_SOURCE_DIR}/src/core/version.h.in ${CMAKE_BINARY_DIR}/generated/core/version.h @ONLY)
include_directories(${CMAKE_BINARY_DIR}/generated)

include(cmake/Shaders.cmake)

# Module ownership is documented in documentation/ARCHITECTURE.md. Each directory has its own CMakeLists.txt,
# owned by the agent responsible for that module. The root file is owned by the lead.
add_subdirectory(shaders)
add_subdirectory(src/core)
add_subdirectory(src/gpu)
add_subdirectory(src/capture)
add_subdirectory(src/present)
add_subdirectory(src/upscale)
add_subdirectory(src/cursor)
if(EXISTS ${CMAKE_SOURCE_DIR}/src/diag/frame_recorder.cpp)
    add_subdirectory(src/diag)
endif()
add_subdirectory(src/overlay)
add_subdirectory(src/models)
add_subdirectory(src/neural)
add_subdirectory(src/relief)
add_subdirectory(src/engine)

# Neural rendering bridge (GPL-3.0-only, see THIRD_PARTY_NOTICES.md): built as the separate MoonSR_NrBridge.dll that
# MoonSR.exe loads at run time. Its folder also holds the ABI template and the bridge tests.
if(EXISTS ${CMAKE_SOURCE_DIR}/addons/moonsr_nr_bridge/CMakeLists.txt)
    add_subdirectory(addons/moonsr_nr_bridge EXCLUDE_FROM_ALL)
endif()

foreach(optional_dir src/framegen tools/fgbench tools/imgtool src/app tools/bench tools/cli tools/testpattern tools/gpuload tests)
    if(EXISTS ${CMAKE_SOURCE_DIR}/${optional_dir}/CMakeLists.txt)
        add_subdirectory(${optional_dir})
    endif()
endforeach()

if(TARGET MoonSR AND TARGET MoonSR_NrBridge)
    add_dependencies(MoonSR MoonSR_NrBridge)
endif()
