# ssr_hiz case：屏幕空间反射与层次遍历
meow_add_case(ssr_hiz
    SOURCES
        src/renderer.cpp
        src/case_ui.cpp
        src/timing_items.cpp
    DESKTOP_ENTRY
        src/main.cpp
    ANDROID_ENTRY
        src/android_main.cpp
    SHADERS
        shaders/scene.vert
        shaders/gbuffer.frag
        shaders/fullscreen.vert
        shaders/present.frag
        shaders/reflect.frag
        shaders/pyramid.comp
)
