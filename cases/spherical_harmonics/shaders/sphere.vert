#version 450
#extension GL_GOOGLE_include_directive : require

#include "environment_common.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 outWorldNormal;

void main()
{
    outWorldNormal = inNormal;
    gl_Position = sb.viewProjection * vec4(inPosition, 1.0);
}
