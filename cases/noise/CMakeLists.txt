# noise case：程序化噪声与多倍频叠加
meow_add_case(noise
    SOURCES
        src/renderer.cpp
        src/case_ui.cpp
        src/timing_items.cpp
    DESKTOP_ENTRY
        src/main.cpp
    ANDROID_ENTRY
        src/android_main.cpp
    SHADERS
        shaders/fullscreen.vert
        shaders/noise.frag
)
