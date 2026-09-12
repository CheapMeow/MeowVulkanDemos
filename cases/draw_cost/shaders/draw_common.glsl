// 实例数据、顺序缓冲与材质常量的共用声明
#ifndef DRAW_COST_COMMON_GLSL
#define DRAW_COST_COMMON_GLSL

struct InstanceData {
    vec4 positionScale;  // xyz 世界位置, w 半边长
    vec4 material;       // x 材质编号, yzw 保留
};

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 cameraPosition;
    vec4 frameParams;  // x 保留, yzw 保留
} scene;

layout(set = 0, binding = 1) readonly buffer InstanceBuffer {
    InstanceData instances[];
} instanceBuffer;

layout(set = 0, binding = 2) readonly buffer OrderBuffer {
    uint indices[];
} orderBuffer;

// 材质只用一个颜色表示，真实的材质切换对应换一整套描述符集
layout(set = 1, binding = 0) uniform MaterialBuffer {
    vec4 color;
} material;

#endif
