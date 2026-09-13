#version 450

// 两个独立渲染通道的路径：光照通道从主存里读回几何缓冲

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 2) uniform sampler2D albedoTexture;
layout(set = 0, binding = 3) uniform sampler2D normalTexture;

void main()
{
    const vec3 albedo = texture(albedoTexture, inUv).rgb;
    const vec3 normal = normalize(texture(normalTexture, inUv).xyz);

    const vec3 light = normalize(vec3(0.45, 0.7, 0.6));
    const float diffuse = max(dot(normal, light), 0.0);
    vec3 result = albedo * (0.35 + 0.85 * diffuse);

    result = result / (result + vec3(1.0));
    outColor = vec4(pow(result, vec3(1.0 / 2.2)), 1.0);
}
