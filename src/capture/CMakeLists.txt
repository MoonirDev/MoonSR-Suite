# Owner: gpu-rendering-engineer (header contract capture_source.h is owned by the lead)
add_library(moonsr_capture STATIC
    capture_source.h
    wgc_capture.cpp
    window_enum.cpp
)
target_link_libraries(moonsr_capture PUBLIC moonsr_gpu PRIVATE windowsapp dwmapi)
