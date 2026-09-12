#version 450
#extension GL_GOOGLE_include_directive : require

#include "draw_common.glsl"

layout(location = 0) in vec2 inUv;

layout(location = 0) out vec4 outColor;

void main()
{
    // 小方块本身只有几个像素，着色开销可以忽略，测到的差异来自命令本身
    outColor = vec4(material.color.rgb, 1.0);
}
