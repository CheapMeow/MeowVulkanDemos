#version 450

// 一个渲染通道两个子通道的路径：几何缓冲作为输入附件，全程留在片上存储里

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput albedoInput;
layout(input_attachment_index = 1, set = 0, binding = 1) uniform subpassInput normalInput;

void main()
{
    const vec3 albedo = subpassLoad(albedoInput).rgb;
    const vec3 normal = normalize(subpassLoad(normalInput).xyz);

    const vec3 light = normalize(vec3(0.45, 0.7, 0.6));
    const float diffuse = max(dot(normal, light), 0.0);
    vec3 result = albedo * (0.35 + 0.85 * diffuse);

    result = result / (result + vec3(1.0));
    outColor = vec4(pow(result, vec3(1.0 / 2.2)), 1.0);
}
