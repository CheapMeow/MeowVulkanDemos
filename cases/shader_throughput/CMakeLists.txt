# shader_throughput case：着色吞吐与采样优化的微基准
meow_add_case(shader_throughput
    SOURCES
        src/renderer.cpp
        src/case_ui.cpp
        src/timing_items.cpp
    DESKTOP_ENTRY
        src/main.cpp
    ANDROID_ENTRY
        src/android_main.cpp
    SHADERS
        shaders/throughput.comp
        shaders/throughput_half.comp
        shaders/fullscreen.vert
        shaders/display.frag
    SHADER_INCLUDES
        shaders/throughput_common.glsl
)
