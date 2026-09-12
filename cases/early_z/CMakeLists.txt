# early_z case：early-Z、深度预通道与 discard
meow_add_case(early_z
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
        shaders/quad.vert
        shaders/prepass.frag
        shaders/shade.frag
    SHADER_INCLUDES
        shaders/quad_common.glsl
)
