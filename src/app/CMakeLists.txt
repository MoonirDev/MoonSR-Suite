# Owner: ui-engineer
add_executable(MoonSR WIN32
    main.cpp
    main_window.cpp
    main_window.h
    nr_import.cpp
    nr_import.h
    pages.cpp
    pages.h
    settings.cpp
    settings.h
    resource.h
    ui/theme.cpp
    ui/theme.h
    ui/gfx.cpp
    ui/gfx.h
    ui/controls.cpp
    ui/controls.h
    ui/number_box.cpp
    ui/retro_controls.cpp
    ui/theme_file.cpp
    ui/theme_file.h
    ui/page.cpp
    ui/page.h
    ui/popup.cpp
    ui/dialog.cpp
    ui/dialog.h
    ui/order_list.cpp
    ui/popup.h
    ui/view.cpp
    ui/view.h
    app.rc
    app.manifest
)
set_source_files_properties(app.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/assets/brand/moonsr.ico")
target_link_libraries(MoonSR PRIVATE moonsr_engine d2d1 dwrite dwmapi user32 gdi32 ole32 advapi32 shell32 comctl32 winmm)
