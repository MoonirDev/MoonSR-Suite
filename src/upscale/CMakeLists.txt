# Owner: lead (infrastructure); upscaling-engineer adds algorithm sources here later.
add_library(moonsr_upscale STATIC
    scale_pass.cpp
    scale_pass.h
    alsr_upscaler.cpp
    alsr_upscaler.h
    alsr_learn.cpp
    alsr_learn.h
)
# moonsr_framegen: Moon ALSR consumes the generator's motion field (MotionFieldView).
target_link_libraries(moonsr_upscale PUBLIC moonsr_gpu moonsr_framegen PRIVATE moonsr_shaders)
