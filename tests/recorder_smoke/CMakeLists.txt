# Console smoke test for src/diag (FrameRecorder). Writes under benchmarks/output/recorder_smoke/.
add_executable(moonsr_recorder_smoke main.cpp)
target_link_libraries(moonsr_recorder_smoke PRIVATE moonsr_diag moonsr_gpu ole32 windowscodecs)
target_compile_definitions(moonsr_recorder_smoke PRIVATE MOONSR_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}")
