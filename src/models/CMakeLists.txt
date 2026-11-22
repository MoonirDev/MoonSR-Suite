# Owner: lead (contract); implementation by a development engineer
add_library(moonsr_models STATIC
    model_store.cpp
    model_store.h
)
target_link_libraries(moonsr_models PUBLIC moonsr_core PRIVATE wintrust crypt32 bcrypt version)
