# Console smoke test for src/overlay (StatsOverlay frame counter). Renders offscreen; needs a GPU but no desktop interaction.
add_executable(moonsr_overlay_smoke main.cpp)
target_link_libraries(moonsr_overlay_smoke PRIVATE moonsr_overlay moonsr_gpu ole32 windowscodecs)
target_compile_definitions(moonsr_overlay_smoke PRIVATE MOONSR_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}")
