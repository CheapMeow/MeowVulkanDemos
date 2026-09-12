#version 450
#extension GL_GOOGLE_include_directive : require

#include "scene_common.glsl"

// 地面只用位置：两层共用同一张网格，颜色由分层编号决定，不参与光照
layout(location = 0) in vec3 inPosition;

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) flat out float outLayer;

void main()
{
    InstanceData instance = instanceBuffer.instances[gl_InstanceIndex];

    // rotation.y 是地面分层编号：0 是下层，1 是上层。两层共用同一张网格，
    // 上层只沿 Y 轴抬高一个间距，因此两者的形状与投影完全一致
    float layer = instance.rotation.y;
    vec3 localPosition = inPosition + vec3(0.0, scene.groundParams.x * layer, 0.0);
    vec3 worldPosition = localPosition * instance.positionScale.w + instance.positionScale.xyz;

    outWorldPosition = worldPosition;
    outLayer = layer;

    gl_Position = scene.viewProj * vec4(worldPosition, 1.0);
}
