#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec3 outWorld;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 viewportParams;
    vec4 modeParams;       // x 描边方式, y 深度通道开关, z 法线通道开关, w 外扩距离
    vec4 outlineParams;    // x 阈值, y 线宽, z 是否按屏幕距离缩放, w 保留
} scene;

layout(set = 0, binding = 1) readonly buffer InstanceBuffer {
    vec4 instances[];
} instanceBuffer;

// 双 Pass 外扩：把顶点沿法线推出去，配合正面剔除画出包在物体外面的一圈
void main()
{
    const vec4 centerSize = instanceBuffer.instances[gl_InstanceIndex * 2];
    const vec3 position = inPosition * centerSize.w;
    const vec3 extruded = position + inNormal * scene.modeParams.w;

    const vec3 world = centerSize.xyz + extruded;
    gl_Position = scene.viewProjection * vec4(world, 1.0);

    outNormal = inNormal;
    outWorld = world;
}
