# Owner: lead (contract); implementation by a development engineer
add_library(moonsr_diag STATIC
    frame_recorder.cpp
    frame_recorder.h
)
target_link_libraries(moonsr_diag PUBLIC moonsr_gpu PRIVATE windowscodecs ole32)
