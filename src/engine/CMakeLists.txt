# Owner: lead
add_library(moonsr_engine STATIC
    engine.cpp
    engine.h
    remote_stages.cpp
    remote_stages.h
)
target_link_libraries(moonsr_engine PUBLIC moonsr_core moonsr_upscale moonsr_framegen moonsr_models moonsr_neural moonsr_relief moonsr_aa PRIVATE moonsr_capture moonsr_present moonsr_cursor moonsr_diag moonsr_overlay moonsr_gpu ole32)
