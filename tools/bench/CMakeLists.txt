# Owner: benchmark-specialist. Offline upscaling benchmark harness (quality metrics + GPU timing).
add_executable(moonsr_bench
    main.cpp
    common.h
    image_io.cpp
    imageops.cpp
    imageops.h
    gen.cpp
    gen.h
    metrics.cpp
    metrics.h
    gpu_runner.cpp
    gpu_runner.h
    report.cpp
    report.h
    selftest.cpp
)
target_link_libraries(moonsr_bench PRIVATE moonsr_upscale moonsr_gpu moonsr_core windowscodecs ole32 gdi32 user32 advapi32)
