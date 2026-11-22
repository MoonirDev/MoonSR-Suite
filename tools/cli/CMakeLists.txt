# Owner: lead
add_executable(moonsr_cli main.cpp)
target_link_libraries(moonsr_cli PRIVATE moonsr_engine)
