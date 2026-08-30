// 相机与实例数据的共用声明
#ifndef SCENE_COMMON_GLSL
#define SCENE_COMMON_GLSL

struct InstanceData {
    vec4 positionScale;  // xyz 世界位置, w 统一缩放
    vec4 rotation;       // x 绕 Y 轴旋转角度, yzw 保留
};

layout(set = 0, binding = 0) uniform CameraBuffer {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    vec4 cameraPosition;
    vec4 frustumPlanes[6];  // xyz 法线, w 常数项, 指向视锥内部为正
    vec4 cullParams;        // x 实例总数, y 模型包围球半径, z 光源数量, w 保留
} camera;

layout(set = 0, binding = 1) readonly buffer InstanceBuffer {
    InstanceData instances[];
} instanceBuffer;

mat3 rotationAroundY(float angle)
{
    float s = sin(angle);
    float c = cos(angle);
    return mat3(c, 0.0, -s,
                0.0, 1.0, 0.0,
                s, 0.0, c);
}

#endif
