# Console smoke test for src/models (ModelStore): inspects and imports stand-in DLLs as data, never loads them.
add_executable(moonsr_model_store_smoke main.cpp)
target_link_libraries(moonsr_model_store_smoke PRIVATE moonsr_models psapi)
target_compile_definitions(moonsr_model_store_smoke PRIVATE MOONSR_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}")
