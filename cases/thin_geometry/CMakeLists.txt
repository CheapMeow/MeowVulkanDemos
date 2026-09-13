# thin_geometry case：亚像素细物体、多重采样与后处理抗锯齿
meow_add_case(thin_geometry
    SOURCES
        src/renderer.cpp
        src/case_ui.cpp
        src/timing_items.cpp
    DESKTOP_ENTRY
        src/main.cpp
    ANDROID_ENTRY
        src/android_main.cpp
    SHADERS
        shaders/rod.vert
        shaders/rod.frag
        shaders/fullscreen.vert
        shaders/copy.frag
        shaders/fxaa.frag
)
