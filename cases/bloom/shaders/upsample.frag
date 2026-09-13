#version 450

// 上一级的结果放大之后叠加到本级上，叠加用固定功能混合完成

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform BloomBuffer {
    vec4 viewportParams;
    vec4 modeParams;
    vec4 miscParams;
} bloom;

layout(set = 0, binding = 1) uniform sampler2D sourceTexture;

void main()
{
    const vec2 texel = 1.0 / vec2(textureSize(sourceTexture, 0));
    // 放大时取四个邻域求平均，避免出现方块
    vec3 color = texture(sourceTexture, inUv + vec2(-texel.x, -texel.y) * 0.5).rgb;
    color += texture(sourceTexture, inUv + vec2(texel.x, -texel.y) * 0.5).rgb;
    color += texture(sourceTexture, inUv + vec2(-texel.x, texel.y) * 0.5).rgb;
    color += texture(sourceTexture, inUv + vec2(texel.x, texel.y) * 0.5).rgb;
    outColor = vec4(color * 0.25, 1.0);
}
