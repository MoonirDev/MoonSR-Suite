# Owner: gpu-rendering-engineer. Console smoke test for the gpu, capture and present modules.
add_executable(moonsr_gpu_smoke main.cpp)
target_link_libraries(moonsr_gpu_smoke PRIVATE moonsr_gpu moonsr_capture moonsr_present ole32 dwmapi)
