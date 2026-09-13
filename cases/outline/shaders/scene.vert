#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec3 outWorld;
layout(location = 2) flat out vec3 outColor;

layout(set = 0, binding = 0) uniform SceneBuffer {
    mat4 viewProjection;
    vec4 viewportParams;   // xy 视口尺寸, zw 保留
    vec4 modeParams;       // x 描边方式, y 深度通道开关, z 法线通道开关, w 外扩距离
    vec4 outlineParams;    // x 阈值, y 线宽, z 是否按屏幕距离缩放, w 保留
} scene;

layout(set = 0, binding = 1) readonly buffer InstanceBuffer {
    vec4 instances[];  // 每个实例两个 vec4：rect = (中心 xyz, 尺寸), data = (rgb, 种类)
} instanceBuffer;

void main()
{
    const vec4 centerSize = instanceBuffer.instances[gl_InstanceIndex * 2];
    const vec4 data = instanceBuffer.instances[gl_InstanceIndex * 2 + 1];

    const vec3 world = centerSize.xyz + inPosition * centerSize.w;
    gl_Position = scene.viewProjection * vec4(world, 1.0);

    outNormal = inNormal;
    outWorld = world;
    outColor = data.rgb;
}
