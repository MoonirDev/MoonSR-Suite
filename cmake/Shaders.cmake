# HLSL build support. Shaders are compiled with fxc (Shader Model 5.0, the D3D11 FL 11_0 baseline)
# into C headers that define `const BYTE <varname>[]`, and embedded in the binaries.

find_program(MOONSR_FXC fxc
    HINTS "$ENV{WindowsSdkVerBinPath}/x64" "$ENV{WindowsSdkDir}/bin/$ENV{WindowsSDKVersion}/x64"
    REQUIRED)

set(MOONSR_SHADER_GEN_DIR ${CMAKE_BINARY_DIR}/shaders_gen)
file(MAKE_DIRECTORY ${MOONSR_SHADER_GEN_DIR})
set_property(GLOBAL PROPERTY MOONSR_SHADER_OUTPUTS "")

# moonsr_add_shader(<file relative to shaders/> <profile> <variable name> [ENTRY <entry>] [DEFINES A=1 B ...])
# Produces ${MOONSR_SHADER_GEN_DIR}/<variable name>.h
function(moonsr_add_shader FILE PROFILE VARNAME)
    cmake_parse_arguments(ARG "" "ENTRY" "DEFINES" ${ARGN})
    if(NOT ARG_ENTRY)
        set(ARG_ENTRY main)
    endif()
    set(src ${CMAKE_SOURCE_DIR}/shaders/${FILE})
    set(out ${MOONSR_SHADER_GEN_DIR}/${VARNAME}.h)
    set(defs "")
    foreach(d ${ARG_DEFINES})
        list(APPEND defs /D ${d})
    endforeach()
    file(GLOB_RECURSE includes CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/shaders/*.hlsli)
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${MOONSR_FXC} /nologo /T ${PROFILE} /E ${ARG_ENTRY} /O3 /WX
                /I ${CMAKE_SOURCE_DIR}/shaders ${defs}
                /Vn ${VARNAME} /Fh ${out} ${src}
        DEPENDS ${src} ${includes}
        COMMENT "fxc ${FILE} -> ${VARNAME}.h"
        VERBATIM)
    set_property(GLOBAL APPEND PROPERTY MOONSR_SHADER_OUTPUTS ${out})
endfunction()

# Call once after all moonsr_add_shader calls. Creates the INTERFACE target `moonsr_shaders`.
function(moonsr_finalize_shaders)
    get_property(outs GLOBAL PROPERTY MOONSR_SHADER_OUTPUTS)
    add_custom_target(moonsr_shaders_gen DEPENDS ${outs})
    add_library(moonsr_shaders INTERFACE)
    add_dependencies(moonsr_shaders moonsr_shaders_gen)
    target_include_directories(moonsr_shaders INTERFACE ${MOONSR_SHADER_GEN_DIR})
endfunction()
