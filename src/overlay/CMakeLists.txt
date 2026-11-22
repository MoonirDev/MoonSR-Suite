# Owner: lead (contract); implementation by a development engineer
add_library(moonsr_overlay STATIC
    stats_overlay.cpp
    stats_overlay.h
)
target_link_libraries(moonsr_overlay PUBLIC moonsr_gpu PRIVATE moonsr_shaders gdi32 user32)
