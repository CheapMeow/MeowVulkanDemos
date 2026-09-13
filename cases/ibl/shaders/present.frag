#version 450

// 输出：方块阵的结果直接画到交换链上

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 5) uniform sampler2D sourceTexture;

void main()
{
    outColor = vec4(texture(sourceTexture, inUv).rgb, 1.0);
}
