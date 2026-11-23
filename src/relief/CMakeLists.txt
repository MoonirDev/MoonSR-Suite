# Owner: upscaling-engineer (algorithms), shader-engineer (HLSL under shaders/relief/). Contract header relief_pass.h: lead.
# Smart parallax (screen-space relief). State: T1 relief shading, first iteration (M1 + M2); parallax not built.
add_library(moonsr_relief STATIC
    relief_pass.cpp
    relief_pass.h
)
# moonsr_framegen is linked for the full MotionFieldView type (frame_generator.h); relief only reads the view, it never
# starts or drives a generator (lead decision D7).
target_link_libraries(moonsr_relief PUBLIC moonsr_gpu PRIVATE moonsr_framegen moonsr_shaders)
