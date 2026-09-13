#version 450

// 一次采样多个层级：把整条降采样链上的每一级按权重在一次里合成出来

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D level0;
layout(set = 0, binding = 1) uniform sampler2D level1;
layout(set = 0, binding = 2) uniform sampler2D level2;
layout(set = 0, binding = 3) uniform sampler2D level3;
layout(set = 0, binding = 4) uniform sampler2D level4;

layout(push_constant) uniform CombineParams {
    vec4 params;   // x 泛光强度, yzw 保留
} combine;

void main()
{
    // 层级越粗权重越低，模拟上一级叠加下来的结果
    vec3 color = texture(level0, inUv).rgb;
    color += texture(level1, inUv).rgb * 0.8;
    color += texture(level2, inUv).rgb * 0.6;
    color += texture(level3, inUv).rgb * 0.4;
    color += texture(level4, inUv).rgb * 0.2;
    outColor = vec4(color * combine.params.x, 1.0);
}
