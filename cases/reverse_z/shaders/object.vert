#version 450
#extension GL_GOOGLE_include_directive : require

#include "scene_common.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec3 outWorldNormal;
layout(location = 2) out vec2 outUv;

void main()
{
    InstanceData instance = instanceBuffer.instances[gl_InstanceIndex];
    mat3 rotation = rotationAroundY(instance.rotation.x);
    vec3 worldPosition = rotation * (inPosition * instance.positionScale.w) + instance.positionScale.xyz;

    outWorldPosition = worldPosition;
    outWorldNormal = rotation * inNormal;
    outUv = inUv;

    gl_Position = scene.viewProj * vec4(worldPosition, 1.0);
}
