#version 450
#extension GL_GOOGLE_include_directive : require

#include "scene_common.glsl"

layout(location = 0) in vec3 inPosition;

// 阴影通道按级逐个渲染，当前级由推送常量给出
layout(push_constant) uniform PushConstants {
    int cascadeIndex;
} push;

void main()
{
    InstanceData instance = instanceBuffer.instances[gl_InstanceIndex];
    mat3 rotation = rotationAroundY(instance.rotation.x);
    vec3 worldPosition = rotation * (inPosition * instance.positionScale.w) + instance.positionScale.xyz;

    gl_Position = scene.lightViewProj[push.cascadeIndex] * vec4(worldPosition, 1.0);
}
