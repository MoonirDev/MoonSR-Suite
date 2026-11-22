# Owner: lead
add_executable(moonsr_testpattern main.cpp)
target_link_libraries(moonsr_testpattern PRIVATE user32 gdi32)
