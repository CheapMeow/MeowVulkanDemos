#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneColor;

// 合成通道：没有后处理抗锯齿时按原样取回光栅化的结果
void main()
{
    outColor = vec4(texture(sceneColor, inUv).rgb, 1.0);
}
