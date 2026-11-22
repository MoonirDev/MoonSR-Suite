# Owner: gpu-rendering-engineer (header contract cursor_overlay.h is owned by the lead)
add_library(moonsr_cursor STATIC
    cursor_overlay.cpp
    cursor_overlay.h
)
target_link_libraries(moonsr_cursor PUBLIC moonsr_gpu PRIVATE moonsr_shaders magnification)
