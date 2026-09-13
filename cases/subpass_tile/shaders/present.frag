#version 450

// 最终把中间结果搬到交换链上，界面也画在这个通道里

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 4) uniform sampler2D sourceTexture;

void main()
{
    outColor = vec4(texture(sourceTexture, inUv).rgb, 1.0);
}
