cmake_minimum_required(VERSION 3.24)
project(MoonSRNrBridge LANGUAGES CXX)

# This target is deliberately excluded from MoonSR's default build. It documents and validates the
# stable bridge ABI; an actual vendor backend is separately built and installed as MoonSR_NrBridge.dll.
add_library(MoonSR_NrBridgeTemplate SHARED bridge_template.cpp)
target_compile_features(MoonSR_NrBridgeTemplate PRIVATE cxx_std_20)
target_include_directories(MoonSR_NrBridgeTemplate PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../sdk/include)
set_target_properties(MoonSR_NrBridgeTemplate PROPERTIES OUTPUT_NAME "MoonSR_NrBridgeTemplate")

get_filename_component(MOONSR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(NR_SOURCE "${CMAKE_CURRENT_LIST_DIR}/neural_rendering")
set(NR_MANAGER_SDK "${CMAKE_CURRENT_LIST_DIR}/third_party")
file(GLOB sdk_bins "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64")
list(SORT sdk_bins COMPARE NATURAL ORDER DESCENDING)
find_program(NR_FXC fxc HINTS ${sdk_bins} REQUIRED)
set(NR_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${NR_GENERATED}")
function(nr_shader name source)
    set(output "${NR_GENERATED}/${name}.h")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${NR_FXC}" /nologo /T cs_5_0 /E CSMain /O3 /Ges /Fh "${output}" /Vn "g_${name}" "${NR_SOURCE}/${source}"
        DEPENDS "${NR_SOURCE}/${source}" VERBATIM)
    set(NR_HEADERS ${NR_HEADERS} "${output}" PARENT_SCOPE)
endfunction()
nr_shader(flow_pack shaders/flow_pack.hlsl)
nr_shader(flow_convert shaders/flow_convert.hlsl)
nr_shader(optiscaler third_party/optiscaler/dlssnr.hlsl)
nr_shader(initialize_guides shaders/initialize_guides.hlsl)
nr_shader(bridge shaders/bridge.hlsl)
nr_shader(amd_colour shaders/amd_colour.hlsl)
nr_shader(picture_shader shaders/picture.hlsl)
nr_shader(scene_change shaders/scene_change.hlsl)
foreach(stage vs ps)
    set(out "${NR_GENERATED}/nr_${stage}.h")
    add_custom_command(OUTPUT "${out}" COMMAND "${NR_FXC}" /nologo /T ${stage}_5_0 /E ${stage} /O3
        /Fh "${out}" /Vn "g_nr_${stage}" "${CMAKE_CURRENT_LIST_DIR}/copy.hlsl"
        DEPENDS "${CMAKE_CURRENT_LIST_DIR}/copy.hlsl" VERBATIM)
    list(APPEND NR_HEADERS "${out}")
endforeach()
add_library(MoonSR_NrBridge SHARED bridge.cpp amd_model.cpp intel_model.cpp
    "${NR_SOURCE}/src/gpu.cpp" "${NR_SOURCE}/src/optical_flow.cpp"
    "${NR_SOURCE}/src/composition.cpp" "${NR_SOURCE}/src/model.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/pipeline.cpp" "${NR_SOURCE}/src/picture.cpp" ${NR_HEADERS})
target_compile_features(MoonSR_NrBridge PRIVATE cxx_std_20)
target_include_directories(MoonSR_NrBridge PRIVATE "${MOONSR_ROOT}/sdk/include" "${NR_SOURCE}/src"
    "${NR_SOURCE}/third_party/nvof" "${NR_GENERATED}" "${NR_MANAGER_SDK}")
target_compile_definitions(MoonSR_NrBridge PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(MoonSR_NrBridge PRIVATE d3d11 d3d12 dxgi d3dcompiler advapi32)
set(FLOW_METRICS "${NR_GENERATED}/fg_flow_metrics.h")
add_custom_command(OUTPUT "${FLOW_METRICS}"
    COMMAND "${NR_FXC}" /nologo /T cs_5_0 /E CSMain /O3 /Ges /Fh "${FLOW_METRICS}" /Vn g_fg_flow_metrics
        "${CMAKE_CURRENT_LIST_DIR}/flow_metrics.hlsl"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/flow_metrics.hlsl" VERBATIM)
set(FLOW_CONSISTENCY "${NR_GENERATED}/fg_flow_consistency.h")
add_custom_command(OUTPUT "${FLOW_CONSISTENCY}"
    COMMAND "${NR_FXC}" /nologo /T cs_5_0 /E main /O3 /Ges /I "${MOONSR_ROOT}/shaders"
        /Fh "${FLOW_CONSISTENCY}" /Vn g_fg_flow_consistency "${MOONSR_ROOT}/shaders/framegen/fg_consistency_cs.hlsl"
    DEPENDS "${MOONSR_ROOT}/shaders/framegen/fg_consistency_cs.hlsl" "${MOONSR_ROOT}/shaders/framegen/fg_common.hlsli" VERBATIM)
add_executable(MoonSR_FlowSmoke EXCLUDE_FROM_ALL flow_smoke.cpp "${FLOW_METRICS}" "${FLOW_CONSISTENCY}"
    "${NR_SOURCE}/src/gpu.cpp" "${NR_SOURCE}/src/optical_flow.cpp" ${NR_HEADERS})
target_compile_features(MoonSR_FlowSmoke PRIVATE cxx_std_20)
target_include_directories(MoonSR_FlowSmoke PRIVATE "${NR_SOURCE}/src" "${NR_SOURCE}/third_party/nvof" "${NR_GENERATED}")
target_compile_definitions(MoonSR_FlowSmoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(MoonSR_FlowSmoke PRIVATE d3d11 d3d12 dxgi d3dcompiler)
option(MOONSR_EXPERIMENTAL_HARDWARE_FLOW "Build the experimental frame-generator hardware motion provider" OFF)
if(MOONSR_EXPERIMENTAL_HARDWARE_FLOW)
    add_library(MoonSR_HardwareFlow STATIC "${NR_SOURCE}/src/gpu.cpp" "${NR_SOURCE}/src/optical_flow.cpp"
        "${FLOW_METRICS}" ${NR_HEADERS})
    target_compile_features(MoonSR_HardwareFlow PUBLIC cxx_std_20)
    target_include_directories(MoonSR_HardwareFlow PUBLIC "${CMAKE_CURRENT_LIST_DIR}" "${NR_SOURCE}/src"
        "${NR_SOURCE}/third_party/nvof" "${NR_GENERATED}")
    target_compile_definitions(MoonSR_HardwareFlow PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(MoonSR_HardwareFlow PUBLIC d3d11 d3d12 dxgi d3dcompiler)
endif()
add_library(MoonSR_NrForwarder SHARED "${NR_SOURCE}/src/model_forwarder.cpp")
set_target_properties(MoonSR_NrForwarder PROPERTIES OUTPUT_NAME "NeuralRendering_nvngx" PREFIX "")
add_dependencies(MoonSR_NrBridge MoonSR_NrForwarder)

# Helper process for NVIDIA neural rendering on a GPU other than the main one (see src/engine/remote_stages.h).
if(TARGET moonsr_engine)
    add_executable(MoonSR_NrHost nr_host.cpp)
    target_compile_features(MoonSR_NrHost PRIVATE cxx_std_20)
    target_include_directories(MoonSR_NrHost PRIVATE "${MOONSR_ROOT}/src")
    target_link_libraries(MoonSR_NrHost PRIVATE moonsr_engine)
    add_dependencies(MoonSR_NrBridge MoonSR_NrHost)
endif()

# Hardware smoke test using the reference's stand-in model; it contains no NVIDIA model data.
add_library(MoonSR_NrFixture SHARED EXCLUDE_FROM_ALL "${NR_SOURCE}/tests/model_runtime_fixture.cpp")
set_target_properties(MoonSR_NrFixture PROPERTIES OUTPUT_NAME "MoonSR_NrFixture")
target_link_libraries(MoonSR_NrFixture PRIVATE d3d12)
add_executable(MoonSR_NrBridgeSmoke EXCLUDE_FROM_ALL bridge_smoke.cpp)
target_compile_features(MoonSR_NrBridgeSmoke PRIVATE cxx_std_20)
target_include_directories(MoonSR_NrBridgeSmoke PRIVATE "${MOONSR_ROOT}/sdk/include" "${NR_SOURCE}/tests" "${MOONSR_ROOT}/src")
target_link_libraries(MoonSR_NrBridgeSmoke PRIVATE d3d11 dxgi)
add_dependencies(MoonSR_NrBridgeSmoke MoonSR_NrBridge MoonSR_NrFixture)
add_library(MoonSR_AmdNrFixture SHARED EXCLUDE_FROM_ALL amd_model_fixture.cpp)
target_compile_features(MoonSR_AmdNrFixture PRIVATE cxx_std_20)
add_executable(MoonSR_AmdNrAbiSmoke EXCLUDE_FROM_ALL amd_model_smoke.cpp amd_model.cpp "${NR_SOURCE}/src/gpu.cpp")
target_compile_features(MoonSR_AmdNrAbiSmoke PRIVATE cxx_std_20)
target_include_directories(MoonSR_AmdNrAbiSmoke PRIVATE "${NR_SOURCE}/src")
target_compile_definitions(MoonSR_AmdNrAbiSmoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(MoonSR_AmdNrAbiSmoke PRIVATE d3d12 dxgi)
add_dependencies(MoonSR_AmdNrAbiSmoke MoonSR_AmdNrFixture)
add_executable(MoonSR_NrSettingsSmoke EXCLUDE_FROM_ALL settings_smoke.cpp "${MOONSR_ROOT}/src/app/settings.cpp")
target_compile_features(MoonSR_NrSettingsSmoke PRIVATE cxx_std_20)
target_include_directories(MoonSR_NrSettingsSmoke PRIVATE "${MOONSR_ROOT}/src")
# Intel backend test: a d5c_chain.dll stand-in with the DLSS5_INTEL C ABI and a smoke test through the real bridge.
add_library(MoonSR_IntelNrFixture SHARED EXCLUDE_FROM_ALL intel_fixture.cpp)
target_compile_features(MoonSR_IntelNrFixture PRIVATE cxx_std_20)
add_executable(MoonSR_IntelNrSmoke EXCLUDE_FROM_ALL intel_smoke.cpp)
target_compile_features(MoonSR_IntelNrSmoke PRIVATE cxx_std_20)
target_include_directories(MoonSR_IntelNrSmoke PRIVATE "${MOONSR_ROOT}/sdk/include")
target_compile_definitions(MoonSR_IntelNrSmoke PRIVATE NOMINMAX)
target_link_libraries(MoonSR_IntelNrSmoke PRIVATE d3d11 dxgi)
add_dependencies(MoonSR_IntelNrSmoke MoonSR_NrBridge MoonSR_IntelNrFixture)
