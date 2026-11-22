# Owner: gpu-rendering-engineer (header contracts gpu_device.h and cross_adapter.h are owned by the lead)
add_library(moonsr_gpu STATIC
    gpu_device.cpp
    gpu_device.h
    display.cpp
    dpi_scope.h
    cross_adapter.cpp
    cross_adapter.h
)
target_link_libraries(moonsr_gpu PUBLIC moonsr_core d3d11 d3d12 dxgi dxguid)
