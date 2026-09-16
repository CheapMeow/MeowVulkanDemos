# texture_filtering case：高阶纹理过滤
meow_add_case(texture_filtering
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
        shaders/fullscreen.vert
        shaders/sample.frag
)
