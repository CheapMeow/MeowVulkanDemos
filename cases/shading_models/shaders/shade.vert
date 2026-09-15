#version 450
#extension GL_GOOGLE_include_directive : require

#include "shading_common.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec3 outWorldNormal;
layout(location = 2) out vec2 outUv;

void main()
{
    outWorldPosition = inPosition;
    outWorldNormal = inNormal;
    outUv = inUv;
    gl_Position = shading.viewProjection * vec4(inPosition, 1.0);
}
