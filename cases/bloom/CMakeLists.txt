# bloom case：泛光与亮部闪烁
meow_add_case(bloom
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
        shaders/scene.frag
        shaders/downsample.frag
        shaders/upsample.frag
        shaders/combine.frag
        shaders/present.frag
)
