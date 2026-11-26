# Owner: upscaling-engineer (algorithms), shader-engineer (HLSL under shaders/aa/). Contract header aa_pass.h: lead.
# Anti-aliasing. State: T1 spatial filter (M1 of documentation/research/anti_aliasing.md); Directional and Temporal not built.
add_library(moonsr_aa STATIC
    aa_pass.cpp
    aa_pass.h
)
target_link_libraries(moonsr_aa PUBLIC moonsr_gpu PRIVATE moonsr_shaders)
