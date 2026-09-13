# alpha_modes case：alpha test、alpha blend 与 alpha to coverage 的边缘对照
meow_add_case(alpha_modes
    SOURCES
        src/renderer.cpp
        src/case_ui.cpp
        src/timing_items.cpp
    DESKTOP_ENTRY
        src/main.cpp
    ANDROID_ENTRY
        src/android_main.cpp
    SHADERS
        shaders/shape.vert
        shaders/shape.frag
        shaders/fullscreen.vert
        shaders/tonemap.frag
)
