# Owner: frame-generation-engineer (header contract frame_generator.h is owned by the lead)
add_library(moonsr_framegen STATIC
    frame_generator.cpp
    frame_generator.h
)
target_link_libraries(moonsr_framegen PUBLIC moonsr_gpu PRIVATE moonsr_shaders)
if(TARGET MoonSR_HardwareFlow)
    target_link_libraries(moonsr_framegen PRIVATE MoonSR_HardwareFlow)
    target_compile_definitions(moonsr_framegen PRIVATE MOONSR_EXPERIMENTAL_HARDWARE_FLOW=1)
endif()
