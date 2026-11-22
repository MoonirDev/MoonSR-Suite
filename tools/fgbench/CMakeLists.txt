# Owner: frame-generation-engineer. Offline frame generation benchmark: synthetic sequences with exact ground truth,
# quality metrics against baselines, GPU timing of PushFrame / Interpolate.
add_executable(moonsr_fgbench
    main.cpp
    fgbench.h
    synth.cpp
    util.cpp
    runner.cpp
)
target_link_libraries(moonsr_fgbench PRIVATE moonsr_framegen moonsr_gpu moonsr_core windowscodecs ole32)
