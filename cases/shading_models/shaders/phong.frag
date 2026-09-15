#version 450
#extension GL_GOOGLE_include_directive : require

#include "shading_common.glsl"

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec2 inUv;

layout(location = 0) out vec4 outColor;

const vec3 ALBEDO = vec3(0.72, 0.66, 0.55);

void main()
{
    const vec3 normal = applyBump(inWorldNormal, inWorldPosition, inUv);
    outColor = vec4(shadeSurface(ALBEDO, normal, inWorldPosition), 1.0);
}
