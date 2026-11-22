# Owner: gpu-rendering-engineer (header contract gpu_device.h is owned by the lead)
add_library(moonsr_gpu STATIC
    gpu_device.cpp
    gpu_device.h
    display.cpp
    dpi_scope.h
)
target_link_libraries(moonsr_gpu PUBLIC moonsr_core d3d11 dxgi dxguid)
