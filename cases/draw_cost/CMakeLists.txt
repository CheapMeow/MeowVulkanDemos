# draw_cost case：绘制命令的固定成本与状态排序
meow_add_case(draw_cost
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
        shaders/small.vert
        shaders/small.frag
    SHADER_INCLUDES
        shaders/draw_common.glsl
)
