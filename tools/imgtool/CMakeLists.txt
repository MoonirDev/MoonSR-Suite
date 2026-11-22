# Owner: development engineer. WIC-only PNG inspection tool (crop, mosaic, diff, strip, info).
add_executable(moonsr_imgtool main.cpp)
target_link_libraries(moonsr_imgtool PRIVATE windowscodecs ole32)
