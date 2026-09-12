# srgb_linear case：sRGB 与线性空间的对照
meow_add_case(srgb_linear
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
        shaders/object.vert
        shaders/object.frag
        shaders/bar.vert
        shaders/bar.frag
)
