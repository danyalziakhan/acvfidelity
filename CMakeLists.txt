cmake_minimum_required(VERSION 3.20)
project(acvfidelity LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# acvfidelity.ini is embedded so the add-on can write it when missing.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/acvfidelity.ini)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/acvfidelity.ini DEFAULT_INI)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/default_ini.h
     CONTENT "#pragma once
const char DEFAULT_INI[] = R\"ini(@DEFAULT_INI@)ini\";
" @ONLY)

add_library(acvfidelity SHARED
    src/main.cpp
    src/common/config.cpp
    src/common/diagnostics.cpp
    src/common/log.cpp
    src/common/shader_patch.cpp
    src/features/ao.cpp
    src/features/lod.cpp
    src/features/ssr.cpp
    src/acvfidelity.rc)
target_include_directories(acvfidelity PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    src/common
    src/features
    deps/reshade/include
    deps/imgui)
target_link_libraries(acvfidelity PRIVATE dxgi shell32 version)
target_compile_definitions(acvfidelity PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(acvfidelity PRIVATE /W3 /utf-8)
set_target_properties(acvfidelity PROPERTIES SUFFIX ".addon64" INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
