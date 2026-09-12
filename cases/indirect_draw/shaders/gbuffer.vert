#version 450
#extension GL_GOOGLE_include_directive : require

#include "scene_common.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;

layout(set = 1, binding = 0) readonly buffer VisibleBuffer {
    uint visibleIndices[];
} visibleBuffer;

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec3 outWorldNormal;
layout(location = 2) out vec2 outUv;

void main()
{
    // 两条绘制路径都通过可见列表间接寻址，保证着色器完全一致
    uint instanceId = visibleBuffer.visibleIndices[gl_InstanceIndex];
    InstanceData instance = instanceBuffer.instances[instanceId];

    mat3 rotation = rotationAroundY(instance.rotation.x);
    vec3 worldPosition = rotation * (inPosition * instance.positionScale.w) + instance.positionScale.xyz;

    outWorldPosition = worldPosition;
    outWorldNormal = rotation * inNormal;
    outUv = inUv;

    gl_Position = camera.viewProj * vec4(worldPosition, 1.0);
}
