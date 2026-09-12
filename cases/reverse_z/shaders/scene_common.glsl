// 相机、地面与实例数据的共用声明
#ifndef REVERSE_Z_SCENE_COMMON_GLSL
#define REVERSE_Z_SCENE_COMMON_GLSL

struct InstanceData {
    vec4 positionScale;  // xyz 世界位置, w 统一缩放
    vec4 rotation;       // x 绕 Y 轴旋转角度, y 地面分层编号, zw 保留
};

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    vec4 cameraPosition;
    vec4 lightDirection;  // xyz 指向光源的单位向量, w 保留
    vec4 groundParams;    // x 上下两层地面的高度间距, yzw 保留
} scene;

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
