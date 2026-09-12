# msaa case：多重采样的覆盖、着色次数与自定义解析
meow_add_case(msaa
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
        shaders/resolve_custom.frag
        shaders/tonemap.frag
)
