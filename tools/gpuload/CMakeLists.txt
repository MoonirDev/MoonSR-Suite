# Owner: lead
add_executable(moonsr_gpuload main.cpp)
target_link_libraries(moonsr_gpuload PRIVATE d3d11 dxgi d3dcompiler)
target_include_directories(moonsr_gpuload PRIVATE ${CMAKE_SOURCE_DIR}/src)
