# Owner: lead
add_library(moonsr_core STATIC
    log.cpp
    log.h
    types.h
)
target_include_directories(moonsr_core PUBLIC ${MOONSR_SRC_DIR})
