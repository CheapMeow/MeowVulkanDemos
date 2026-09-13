#version 450

// 最终输出：延迟路径的光照结果已经写好，前向路径的结果也在这里，界面画在同一个通道里

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 5) uniform sampler2D sourceTexture;

void main()
{
    outColor = vec4(texture(sourceTexture, inUv).rgb, 1.0);
}
