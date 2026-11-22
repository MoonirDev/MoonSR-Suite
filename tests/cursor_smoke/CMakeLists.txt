# Owner: gpu-rendering-engineer. Console smoke test for src/cursor (moves the real cursor; needs an interactive desktop).
add_executable(moonsr_cursor_smoke main.cpp)
target_link_libraries(moonsr_cursor_smoke PRIVATE moonsr_cursor moonsr_gpu ole32 windowscodecs)
target_compile_definitions(moonsr_cursor_smoke PRIVATE MOONSR_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}")
