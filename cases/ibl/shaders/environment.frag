#version 450

// 把程序生成的天空写进一张等距圆柱投影的环境贴图，作为后续预计算的输入

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

#include "sky_common.glsl"

void main()
{
    outColor = vec4(skyColor(uvToDirection(inUv)), 1.0);
}
