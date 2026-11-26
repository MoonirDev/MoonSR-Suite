# Owner: upscaling-engineer (algorithms), shader-engineer (HLSL under shaders/aa/). Contract header aa_pass.h: lead.
# Anti-aliasing. State: spatial tier T1 (pattern-based morphological filter, MLAA family). Directional and Temporal
# modes are accepted by the settings and reported as a spatial fallback; they are not built.
add_library(moonsr_aa STATIC
    aa_pass.cpp
    aa_pass.h
)
target_link_libraries(moonsr_aa PUBLIC moonsr_gpu PRIVATE moonsr_shaders)
