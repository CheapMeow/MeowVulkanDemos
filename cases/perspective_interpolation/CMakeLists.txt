# perspective_interpolation case：透视矫正插值与仿射插值的对照
meow_add_case(perspective_interpolation
    SOURCES
        src/renderer.cpp
        src/scene_setup.cpp
        src/case_ui.cpp
        src/timing_items.cpp
    DESKTOP_ENTRY
        src/main.cpp
    ANDROID_ENTRY
        src/android_main.cpp
    SHADERS
        shaders/interp.vert
        shaders/interp.frag
)
