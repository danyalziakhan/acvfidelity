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
    src/features/light.cpp
    src/features/light_patch.cpp
    src/features/lod.cpp
    src/features/lod_control.cpp
    src/features/lod_hook.cpp
    src/features/lod_fade.cpp
    src/features/grass.cpp
    src/features/ssr.cpp
    src/features/ssr_angle.cpp
    src/features/descriptors.cpp
    src/features/scene_state.cpp
    src/features/scene_inject.cpp
    src/features/ssr2.cpp
    src/features/ssil.cpp
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

# The scene hook's compute shaders (surface reflections, indirect lighting and the shared passes), compiled by fxc
# into headers.
file(GLOB FXC_CANDIDATES "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/fxc.exe")
list(SORT FXC_CANDIDATES ORDER DESCENDING)
if(FXC_CANDIDATES)
    list(GET FXC_CANDIDATES 0 FXC)
else()
    find_program(FXC NAMES fxc REQUIRED)
endif()
set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/features/shaders)
set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/generated/shaders)
set(SHADER_HEADERS)
foreach(entry IN ITEMS scene:CSSetup scene:CSPrepare scene:CSDepthHalf scene:CSPyramidDown scene:CSCapture
        scene:CSCaptureDown scene:CSTemporal ssr2:CSTrace ssr2:CSSpatial ssr2:CSUpsample
        ssil:SSILTrace ssil:SSILTemporal ssil:SSILSpatial ssil:SSILFinal)
    string(REPLACE ":" ";" parts ${entry})
    list(GET parts 0 file)
    list(GET parts 1 name)
    add_custom_command(OUTPUT ${SHADER_OUT}/${name}.h
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUT}
        COMMAND ${FXC} /nologo /WX /O3 /T cs_5_1 /E ${name} /Vn ${name} /Fh ${SHADER_OUT}/${name}.h ${SHADER_DIR}/${file}.hlsl
        DEPENDS ${SHADER_DIR}/${file}.hlsl ${SHADER_DIR}/common.hlsli
        VERBATIM)
    list(APPEND SHADER_HEADERS ${SHADER_OUT}/${name}.h)
endforeach()
target_sources(acvfidelity PRIVATE ${SHADER_HEADERS})
target_include_directories(acvfidelity PRIVATE ${SHADER_OUT})

