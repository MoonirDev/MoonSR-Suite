# Owner: gpu-rendering-engineer. Correctness test and timing for CrossAdapterLink (src/gpu/cross_adapter.h).
add_executable(moonsr_cross_adapter_smoke main.cpp)
target_link_libraries(moonsr_cross_adapter_smoke PRIVATE moonsr_gpu)
