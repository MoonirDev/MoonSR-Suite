# Owner: gpu-rendering-engineer (header contract presenter.h is owned by the lead)
add_library(moonsr_present STATIC
    presenter.cpp
    presenter.h
)
target_link_libraries(moonsr_present PUBLIC moonsr_gpu PRIVATE dwmapi)
