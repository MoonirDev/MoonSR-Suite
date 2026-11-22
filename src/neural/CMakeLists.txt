add_library(moonsr_neural STATIC
    neural_bridge.cpp
    neural_bridge.h
    neural_settings.h
)
target_include_directories(moonsr_neural PUBLIC ${CMAKE_SOURCE_DIR}/sdk/include)
target_link_libraries(moonsr_neural PUBLIC moonsr_core moonsr_gpu)
