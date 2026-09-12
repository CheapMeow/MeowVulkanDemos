# texture_lod case：多级纹理、LOD 与各向异性过滤
meow_add_case(texture_lod
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
        shaders/lod.vert
        shaders/lod.frag
)
