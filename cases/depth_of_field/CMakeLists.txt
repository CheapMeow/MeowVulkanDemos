# depth_of_field case：景深与散景
meow_add_case(depth_of_field
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
        shaders/scene.vert
        shaders/scene.frag
        shaders/dof.frag
)
