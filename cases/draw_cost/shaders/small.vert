#version 450
#extension GL_GOOGLE_include_directive : require

#include "draw_common.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUv;

layout(location = 0) out vec2 outUv;
layout(location = 1) out vec3 outDebug;

void main()
{
    uint slot = uint(gl_InstanceIndex);
    uint index = orderBuffer.indices[slot];
    InstanceData instance = instanceBuffer.instances[index];

    vec3 worldPosition = instance.positionScale.xyz + vec3(inPosition.xy * instance.positionScale.w, 0.0);

    outUv = inUv;
    outDebug = worldPosition;
    gl_Position = scene.viewProjection * vec4(worldPosition, 1.0);
}
