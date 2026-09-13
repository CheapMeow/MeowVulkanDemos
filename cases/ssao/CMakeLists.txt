# ssao case：屏幕空间环境光遮蔽
meow_add_case(ssao
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
        shaders/scene.frag
        shaders/fullscreen.vert
        shaders/ssao.frag
        shaders/composite.frag
)
