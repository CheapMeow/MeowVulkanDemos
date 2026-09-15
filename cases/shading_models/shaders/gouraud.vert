#version 450
#extension GL_GOOGLE_include_directive : require

#include "shading_common.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;

layout(location = 0) out vec3 outColor;

const vec3 ALBEDO = vec3(0.72, 0.66, 0.55);

void main()
{
    // 光照在顶点上算一次，三个顶点的颜色再线性插值到整片三角形
    outColor = shadeSurface(ALBEDO, normalize(inNormal), inPosition);
    gl_Position = shading.viewProjection * vec4(inPosition, 1.0);
}
